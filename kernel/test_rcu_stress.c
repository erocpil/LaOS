/*
 * test_rcu_stress.c - configurable preemptible-RCU and list stress selftest
 *
 * Readers deliberately schedule while inside an RCU read-side critical
 * section.  This deterministically exercises rcu_note_context_switch() and
 * blocked_tasks instead of relying on a timer interrupt landing in a busy
 * loop.  The writer runs a plain grace period, then publishes, observes,
 * removes and reclaims one list node per round.
 */

#include "test_rcu_stress.h"
#include "config.h"
#include "selftest.h"
#include "rcu.h"
#include "cpu.h"
#include "sched.h"
#include "printf.h"
#include "string.h"
#include "heap.h"

#if CONFIG_RCU

#define RCU_STRESS_MAGIC       0x5243555354524553ULL
#define RCU_STRESS_ROUNDS_MAX  10000U
#define RCU_STRESS_TIMEOUT_MAX 60000U

struct rcu_stress_node {
	struct list_node node;
	uint64_t magic;
	uint64_t sequence;
	uint64_t inverse;
};

struct rcu_stress_reader {
	uint32_t index;
	uint32_t cpu;
	char name[THREAD_NAME_MAX];
};

static struct list_node stress_list = LIST_NODE_INIT(stress_list);
static struct rcu_stress_reader reader_args[MAX_CPUS];
static struct thread *reader_threads[MAX_CPUS];
static struct thread *writer_thread;
static struct rcu_instance_metric stress_metric;

static uint32_t configured_rounds = 100;
static uint32_t configured_readers;
static uint32_t timeout_ticks = 2000;
static uint32_t active_readers;

static volatile uint64_t elapsed_ticks;
static volatile uint64_t completed_rounds;
static volatile uint64_t observed_sequence;
static volatile uint64_t reader_visits;
static volatile uint64_t blocked_hits;
static volatile uint32_t readers_alive;
static volatile bool stop_readers;
static volatile bool writer_exited;
static volatile bool failed;

static uint32_t parse_u32(const char *value, uint32_t fallback)
{
	uint32_t result = 0;

	if (!value || *value == '\0') {
		return fallback;
	}
	for (const char *p = value; *p >= '0' && *p <= '9'; p++) {
		result = result * 10U + (uint32_t)(*p - '0');
	}

	return result ? result : fallback;
}

static void rcu_stress_configure(const char *key, const char *value)
{
	if (strcmp(key, "rounds") == 0) {
		uint32_t rounds = parse_u32(value, 100);
		configured_rounds = rounds > RCU_STRESS_ROUNDS_MAX
			? RCU_STRESS_ROUNDS_MAX : rounds;
	} else if (strcmp(key, "readers") == 0) {
		uint32_t readers = parse_u32(value, 0);
		configured_readers = readers > MAX_CPUS ? MAX_CPUS : readers;
	} else if (strcmp(key, "timeout_ticks") == 0) {
		uint32_t timeout = parse_u32(value, 2000);
		timeout_ticks = timeout > RCU_STRESS_TIMEOUT_MAX
			? RCU_STRESS_TIMEOUT_MAX : timeout;
	}
}

static void rcu_stress_reader(void *arg)
{
	struct rcu_stress_reader *reader = arg;

	/*
	 * Preemptible RCU must preserve this reader across a context switch.
	 * Sleeping once here guarantees the blocked-reader path is exercised
	 * on every participating CPU.  Repeating it indefinitely would let new
	 * readers continuously extend this teaching implementation's global
	 * blocked_tasks wait and turn the test into reader-flood starvation.
	 */
	rcu_read_lock();
	schedule_timeout(1);
	if (get_current()->rcu_blocked) {
		__atomic_add_fetch(&blocked_hits, 1, __ATOMIC_RELAXED);
	}
	rcu_read_unlock();

	while (!__atomic_load_n(&stop_readers, __ATOMIC_ACQUIRE)) {
		struct rcu_stress_node *item;

		rcu_read_lock();
		list_for_each_entry_rcu(item, &stress_list, node) {
			uint64_t magic = __atomic_load_n(&item->magic,
					__ATOMIC_RELAXED);
			uint64_t sequence = __atomic_load_n(&item->sequence,
					__ATOMIC_RELAXED);
			uint64_t inverse = __atomic_load_n(&item->inverse,
					__ATOMIC_RELAXED);

			if (magic != RCU_STRESS_MAGIC || inverse != ~sequence) {
				__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
				__atomic_store_n(&stop_readers, true,
						__ATOMIC_RELEASE);
				continue;
			}
			__atomic_store_n(&observed_sequence, sequence,
					__ATOMIC_RELEASE);
			__atomic_add_fetch(&reader_visits, 1,
					__ATOMIC_RELAXED);
		}
		rcu_read_unlock();

		stress_metric.iters[reader->cpu]++;

		schedule_timeout(1);
	}

	__atomic_sub_fetch(&readers_alive, 1, __ATOMIC_ACQ_REL);
	stress_metric.readers_alive =
		__atomic_load_n(&readers_alive, __ATOMIC_ACQUIRE);
}

static void rcu_stress_writer(void *arg)
{
	(void)arg;

	/* Wait for the deliberately blocked startup readers. */
	synchronize_rcu();

	for (uint64_t round = 1; round <= configured_rounds; round++) {
		struct rcu_stress_node *node;

		if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) {
			break;
		}

		node = kmalloc(sizeof(*node));
		if (!node) {
			kprintf("[rcu_stress] FAIL: allocation at round %llu\n",
					round);
			__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
			break;
		}

		node->magic = RCU_STRESS_MAGIC;
		node->sequence = round;
		node->inverse = ~round;
		list_add_rcu(&node->node, &stress_list);

		while (__atomic_load_n(&observed_sequence,
					__ATOMIC_ACQUIRE) != round &&
				!__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) {
			schedule_timeout(1);
		}

		list_del_rcu(&node->node);
		synchronize_rcu();
		node->magic = 0;
		kfree(node);

		if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) {
			break;
		}
		__atomic_store_n(&completed_rounds, round, __ATOMIC_RELEASE);
	}

	__atomic_store_n(&stop_readers, true, __ATOMIC_RELEASE);
	while (__atomic_load_n(&readers_alive, __ATOMIC_ACQUIRE) > 0) {
		schedule();
	}
	__atomic_store_n(&writer_exited, true, __ATOMIC_RELEASE);
}

static int rcu_stress_prepare(void)
{
	uint64_t online_cpus = __atomic_load_n(&online, __ATOMIC_ACQUIRE);

	if (online_cpus < 2) {
		kprintf("[rcu_stress] FAIL: requires at least 2 online CPUs\n");
		return -1;
	}
	if (online_cpus > MAX_CPUS) {
		online_cpus = MAX_CPUS;
	}

	active_readers = configured_readers
		? configured_readers : (uint32_t)online_cpus;
	if (active_readers > online_cpus) {
		active_readers = (uint32_t)online_cpus;
	}

	list_init(&stress_list);
	memset(reader_args, 0, sizeof(reader_args));
	memset(reader_threads, 0, sizeof(reader_threads));
	memset(&stress_metric, 0, sizeof(stress_metric));
	writer_thread = NULL;
	elapsed_ticks = 0;
	completed_rounds = 0;
	observed_sequence = 0;
	reader_visits = 0;
	blocked_hits = 0;
	readers_alive = 0;
	stop_readers = false;
	writer_exited = false;
	failed = false;

	for (uint32_t i = 0; i < active_readers; i++) {
		struct rcu_stress_reader *reader = &reader_args[i];

		reader->index = i;
		reader->cpu = i;
		ksprintf(reader->name, "rcu-st-r%u", i);
		reader_threads[i] = selftest_create_worker((int)reader->cpu,
				rcu_stress_reader, reader->name, reader);
		if (!reader_threads[i]) {
			for (uint32_t j = 0; j < i; j++) {
				selftest_discard_worker(reader_threads[j]);
			}
			__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
			return -1;
		}
	}

	writer_thread = selftest_create_worker(0, rcu_stress_writer,
			"rcu-st-wr", NULL);
	if (!writer_thread) {
		for (uint32_t i = 0; i < active_readers; i++) {
			selftest_discard_worker(reader_threads[i]);
		}
		__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
		return -1;
	}

	stress_metric.n_readers = active_readers;
	stress_metric.readers_alive = active_readers;
	memcpy(stress_metric.name, "rcu-stress", 11);
	INIT_LIST_NODE(&stress_metric.node);
	list_add_tail(&stress_metric.node, &rcu_metric.head);
	rcu_metric.n_rcu++;

	kprintf("[rcu_stress] prepared: rounds=%u readers=%u timeout=%u\n",
			configured_rounds, active_readers, timeout_ticks);

	return 0;
}

static void rcu_stress_start(void)
{
	__atomic_store_n(&readers_alive, active_readers, __ATOMIC_RELEASE);
	for (uint32_t i = 0; i < active_readers; i++) {
		selftest_start_worker(reader_threads[i]);
	}
	selftest_start_worker(writer_thread);
}

static void rcu_stress_tick(void)
{
	if (__atomic_load_n(&writer_exited, __ATOMIC_ACQUIRE) ||
			__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) {
		return;
	}

	uint64_t elapsed = __atomic_add_fetch(&elapsed_ticks, 1,
			__ATOMIC_RELAXED);
	if (elapsed >= timeout_ticks) {
		kprintf("[rcu_stress] FAIL: timeout after %llu ticks "
				"(completed=%llu/%u readers_alive=%u)\n",
				elapsed,
				__atomic_load_n(&completed_rounds,
					__ATOMIC_ACQUIRE),
				configured_rounds,
				__atomic_load_n(&readers_alive,
					__ATOMIC_ACQUIRE));
		__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
		__atomic_store_n(&stop_readers, true, __ATOMIC_RELEASE);
	}
}

static bool rcu_stress_done(void)
{
	return __atomic_load_n(&failed, __ATOMIC_ACQUIRE) ||
		(__atomic_load_n(&writer_exited, __ATOMIC_ACQUIRE) &&
		 __atomic_load_n(&readers_alive, __ATOMIC_ACQUIRE) == 0);
}

static bool rcu_stress_passed(void)
{
	uint64_t completed = __atomic_load_n(&completed_rounds,
			__ATOMIC_ACQUIRE);
	uint64_t visits = __atomic_load_n(&reader_visits, __ATOMIC_ACQUIRE);
	uint64_t blocked = __atomic_load_n(&blocked_hits, __ATOMIC_ACQUIRE);
	bool ok = !__atomic_load_n(&failed, __ATOMIC_ACQUIRE) &&
		__atomic_load_n(&writer_exited, __ATOMIC_ACQUIRE) &&
		completed == configured_rounds &&
		visits >= configured_rounds &&
		blocked >= active_readers &&
		list_empty(&stress_list);

	kprintf("[rcu_stress] %s: completed=%llu/%u visits=%llu "
			"blocked_hits=%llu list_empty=%d\n",
			ok ? "PASSED" : "FAILED", completed, configured_rounds,
			visits, blocked, list_empty(&stress_list));

	return ok;
}

static const struct selftest rcu_stress_test = {
	.name = "rcu_stress",
	.configure = rcu_stress_configure,
	.prepare = rcu_stress_prepare,
	.start = rcu_stress_start,
	.tick = rcu_stress_tick,
	.done = rcu_stress_done,
	.passed = rcu_stress_passed,
};

#endif

void test_rcu_stress_init(void)
{
#if CONFIG_RCU
	if (selftest_register(&rcu_stress_test) < 0) {
		kprintf("[rcu_stress] WARNING: register failed\n");
	}
#endif
}
