/*
 * test_remote_enqueue.c - verify post-online remote runqueue wakeup
 *
 * The BSP creates a worker for CPU 1 during prepare(), then queues it only
 * when this test starts.  The AP must receive a reschedule IPI and run the
 * worker; relying on a later periodic timer tick is deliberately not enough.
 */

#include "test_remote_enqueue.h"
#include "selftest.h"
#include "cpu.h"
#include "printf.h"
#include "string.h"
#include "ipi.h"

static struct thread *remote_worker;
static volatile bool remote_ran;
static volatile bool remote_failed;
static volatile uint64_t elapsed_ticks;
static uint32_t timeout_ticks = 200;
static int target_cpu = 1;
static int observed_cpu = -1;
static uint64_t reschedule_count_before;

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

static void remote_enqueue_configure(const char *key, const char *value)
{
	if (strcmp(key, "timeout_ticks") == 0) {
		timeout_ticks = parse_u32(value, 200);
	}
}

static void remote_enqueue_worker(void *arg)
{
	(void)arg;

	observed_cpu = cpu_get_ctx()->id;
	__atomic_store_n(&remote_ran, true, __ATOMIC_RELEASE);
}

static int remote_enqueue_prepare(void)
{
	uint64_t online_cpus = __atomic_load_n(&online, __ATOMIC_ACQUIRE);

	remote_worker = NULL;
	remote_ran = false;
	remote_failed = false;
	elapsed_ticks = 0;
	observed_cpu = -1;
	reschedule_count_before = 0;

	if (online_cpus < 2 || !g_cpu_contexts[target_cpu]) {
		kprintf("[remote_enqueue] FAIL: requires at least 2 online CPUs\n");
		remote_failed = true;
		return -1;
	}

	remote_worker = selftest_create_worker(target_cpu,
			remote_enqueue_worker, "remote-enqueue", NULL);
	if (!remote_worker) {
		remote_failed = true;
		return -1;
	}

	kprintf("[remote_enqueue] prepared: target_cpu=%d timeout=%u\n",
			target_cpu, timeout_ticks);
	reschedule_count_before =
		ipi_reschedule_count((uint32_t)target_cpu);

	return 0;
}

static void remote_enqueue_start(void)
{
	/* This is the operation under test: a live BSP dynamically queues work
	 * to an already-online AP. */
	selftest_start_worker(remote_worker);
}

static void remote_enqueue_tick(void)
{
	if (__atomic_load_n(&remote_ran, __ATOMIC_ACQUIRE) ||
			__atomic_load_n(&remote_failed, __ATOMIC_ACQUIRE)) {
		return;
	}

	uint64_t elapsed = __atomic_add_fetch(&elapsed_ticks, 1,
			__ATOMIC_RELAXED);
	if (elapsed >= timeout_ticks) {
		kprintf("[remote_enqueue] FAIL: CPU %d did not run worker "
				"within %llu ticks\n", target_cpu, elapsed);
		__atomic_store_n(&remote_failed, true, __ATOMIC_RELEASE);
	}
}

static bool remote_enqueue_done(void)
{
	return __atomic_load_n(&remote_ran, __ATOMIC_ACQUIRE) ||
		__atomic_load_n(&remote_failed, __ATOMIC_ACQUIRE);
}

static bool remote_enqueue_passed(void)
{
	bool ok = __atomic_load_n(&remote_ran, __ATOMIC_ACQUIRE) &&
		!__atomic_load_n(&remote_failed, __ATOMIC_ACQUIRE) &&
		observed_cpu == target_cpu &&
		ipi_reschedule_count((uint32_t)target_cpu) >
		reschedule_count_before;

	kprintf("[remote_enqueue] %s: target_cpu=%d observed_cpu=%d "
			"reschedule_ipis=%llu\n", ok ? "PASSED" : "FAILED",
			target_cpu, observed_cpu,
			ipi_reschedule_count((uint32_t)target_cpu) -
			reschedule_count_before);

	return ok;
}

static const struct selftest remote_enqueue_test = {
	.name = "remote_enqueue",
	.configure = remote_enqueue_configure,
	.prepare = remote_enqueue_prepare,
	.start = remote_enqueue_start,
	.tick = remote_enqueue_tick,
	.done = remote_enqueue_done,
	.passed = remote_enqueue_passed,
};

void test_remote_enqueue_init(void)
{
	if (selftest_register(&remote_enqueue_test) < 0) {
		kprintf("[remote_enqueue] WARNING: register failed\n");
	}
}
