/*
 * test_rcu_publish.c - bounded cross-CPU RCU list publication selftest
 *
 * The writer initializes one static node, publishes it with list_add_rcu(),
 * waits until the reader observes and validates it, removes it, and waits for
 * a grace period before reusing it.  A per-round handshake makes every round
 * exercise the list link rather than merely hoping a polling reader wins.
 */

#include "test_rcu_publish.h"
#include "selftest.h"
#include "rcu.h"
#include "sched.h"
#include "cpu.h"
#include "printf.h"
#include "string.h"

#define RCU_PUBLISH_MAGIC      0x5243555055424c49ULL
#define RCU_PUBLISH_ROUNDS_MAX 256U

struct rcu_publish_node {
	struct list_node node;
	uint64_t magic;
	uint64_t sequence;
	uint64_t inverse;
};

static struct list_node publish_list = LIST_NODE_INIT(publish_list);
static struct rcu_publish_node publish_node;
static struct thread *reader_thread;
static struct thread *writer_thread;

static volatile uint64_t observed_sequence;
static volatile uint64_t completed_rounds;
static volatile uint64_t elapsed_ticks;
static volatile bool stop_reader;
static volatile bool reader_exited;
static volatile bool writer_exited;
static volatile bool failed;

static uint32_t configured_rounds = 16;
static uint32_t timeout_ticks = 1000;

/*
 * These fixtures let the ARM64 build gate inspect the exact primitives used
 * by the inline list helpers.  They are not called at runtime.
 */
__attribute__((noinline, used))
struct list_node *rcu_order_fixture_load(struct list_node **link)
{
	return __list_LOAD_ACQUIRE(*link);
}

__attribute__((noinline, used))
void rcu_order_fixture_store(struct list_node **link, struct list_node *value)
{
	__list_STORE_RELEASE(*link, value);
}

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

static void rcu_publish_configure(const char *key, const char *value)
{
	if (strcmp(key, "rounds") == 0) {
		uint32_t rounds = parse_u32(value, 16);
		configured_rounds = rounds > RCU_PUBLISH_ROUNDS_MAX
			? RCU_PUBLISH_ROUNDS_MAX : rounds;
	} else if (strcmp(key, "timeout_ticks") == 0) {
		timeout_ticks = parse_u32(value, 1000);
	}
}

static void rcu_publish_reader(void *arg)
{
	(void)arg;

	while (!__atomic_load_n(&stop_reader, __ATOMIC_ACQUIRE)) {
		struct rcu_publish_node *item;

		rcu_read_lock();
		list_for_each_entry_rcu(item, &publish_list, node) {
			uint64_t magic = __atomic_load_n(&item->magic,
					__ATOMIC_RELAXED);
			uint64_t seq = __atomic_load_n(&item->sequence,
					__ATOMIC_RELAXED);
			uint64_t inverse = __atomic_load_n(&item->inverse,
					__ATOMIC_RELAXED);

			if (magic != RCU_PUBLISH_MAGIC || inverse != ~seq) {
				__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
				continue;
			}
			__atomic_store_n(&observed_sequence, seq,
					__ATOMIC_RELEASE);
		}
		rcu_read_unlock();
		schedule_timeout(1);
	}

	__atomic_store_n(&reader_exited, true, __ATOMIC_RELEASE);
}

static void rcu_publish_writer(void *arg)
{
	(void)arg;

	for (uint64_t round = 1; round <= configured_rounds; round++) {
		publish_node.magic = RCU_PUBLISH_MAGIC;
		publish_node.sequence = round;
		publish_node.inverse = ~round;
		list_add_rcu(&publish_node.node, &publish_list);

		while (__atomic_load_n(&observed_sequence,
					__ATOMIC_ACQUIRE) != round) {
			if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) {
				break;
			}
			schedule_timeout(1);
		}

		list_del_rcu(&publish_node.node);
		synchronize_rcu();

		if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) {
			break;
		}
		__atomic_store_n(&completed_rounds, round, __ATOMIC_RELEASE);
	}

	__atomic_store_n(&stop_reader, true, __ATOMIC_RELEASE);
	__atomic_store_n(&writer_exited, true, __ATOMIC_RELEASE);
}

static int rcu_publish_prepare(void)
{
	list_init(&publish_list);
	memset(&publish_node, 0, sizeof(publish_node));
	observed_sequence = 0;
	completed_rounds = 0;
	elapsed_ticks = 0;
	stop_reader = false;
	reader_exited = false;
	writer_exited = false;
	failed = false;

	uint64_t online_cpus = __atomic_load_n(&online, __ATOMIC_ACQUIRE);
	int reader_cpu = online_cpus > 1 ? 1 : 0;

	reader_thread = selftest_create_worker(reader_cpu, rcu_publish_reader,
			"rcu-pub-rd", NULL);
	writer_thread = selftest_create_worker(0, rcu_publish_writer,
			"rcu-pub-wr", NULL);
	if (!reader_thread || !writer_thread) {
		selftest_discard_worker(reader_thread);
		selftest_discard_worker(writer_thread);
		reader_thread = NULL;
		writer_thread = NULL;
		failed = true;
		return -1;
	}

	kprintf("[rcu_publish] prepared: rounds=%u reader_cpu=%d timeout=%u\n",
			configured_rounds, reader_cpu, timeout_ticks);

	return 0;
}

static void rcu_publish_start(void)
{
	selftest_start_worker(reader_thread);
	selftest_start_worker(writer_thread);
}

static void rcu_publish_tick(void)
{
	if (__atomic_load_n(&writer_exited, __ATOMIC_ACQUIRE) &&
			__atomic_load_n(&reader_exited, __ATOMIC_ACQUIRE)) {
		return;
	}

	uint64_t elapsed = __atomic_add_fetch(&elapsed_ticks, 1,
			__ATOMIC_RELAXED);
	if (elapsed >= timeout_ticks) {
		kprintf("[rcu_publish] FAIL: timeout after %llu ticks "
				"(completed=%llu/%u)\n", elapsed,
				__atomic_load_n(&completed_rounds, __ATOMIC_ACQUIRE),
				configured_rounds);
		__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
		__atomic_store_n(&stop_reader, true, __ATOMIC_RELEASE);
	}
}

static bool rcu_publish_done(void)
{
	if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) {
		return true;
	}

	return __atomic_load_n(&writer_exited, __ATOMIC_ACQUIRE) &&
		__atomic_load_n(&reader_exited, __ATOMIC_ACQUIRE);
}

static bool rcu_publish_passed(void)
{
	uint64_t completed = __atomic_load_n(&completed_rounds, __ATOMIC_ACQUIRE);
	bool ok = !__atomic_load_n(&failed, __ATOMIC_ACQUIRE) &&
		completed == configured_rounds && list_empty(&publish_list);

	kprintf("[rcu_publish] %s: completed=%llu/%u list_empty=%d\n",
			ok ? "PASSED" : "FAILED", completed, configured_rounds,
			list_empty(&publish_list));

	return ok;
}

static const struct selftest rcu_publish_test = {
	.name = "rcu_publish",
	.configure = rcu_publish_configure,
	.prepare = rcu_publish_prepare,
	.start = rcu_publish_start,
	.tick = rcu_publish_tick,
	.done = rcu_publish_done,
	.passed = rcu_publish_passed,
};

void test_rcu_publish_init(void)
{
	if (selftest_register(&rcu_publish_test) < 0) {
		kprintf("[rcu_publish] WARNING: register failed\n");
	}
}
