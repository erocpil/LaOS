/*
 * test_sched_stress.c — ARM64 scheduler stress test payload
 *
 * Spawns W kernel worker threads distributed across all online CPUs.
 * Each worker does R rounds of schedule_timeout(1) to force repeated
 * context switches.  Validates:
 *   1. Every worker makes progress (no starvation)
 *   2. Multi-CPU runqueues don't deadlock
 *   3. Preemptive timer IRQ → schedule() path works
 *
 * Controlled by task.conf directive:
 *   @test sched_stress module=test_sched_stress.mo rounds=200 workers=4 timeout_ticks=5000
 */
#include "selftest.h"
#include "thread.h"
#include "sched.h"
#include "printf.h"
#include "cpu.h"
#include "export.h"
#include "string.h"

#define MAX_WORKERS 16

/* ── Per-worker state ───────────────────────────────────── */
struct stress_worker {
	volatile uint64_t rounds_done;
	volatile bool     finished;
	uint32_t          total_rounds;
	uint32_t          cpu;
	char              name[8];
};

/* ── Global test state ──────────────────────────────────── */
static struct stress_worker s_workers[MAX_WORKERS];
static struct thread *s_worker_threads[MAX_WORKERS];
static volatile uint32_t s_cfg_rounds       = 200;
static volatile uint32_t s_cfg_workers      = 0;   /* 0 = auto */
static volatile uint32_t s_timeout_ticks    = 5000;
static volatile uint64_t s_elapsed;
static volatile uint64_t s_last_progress;
static volatile bool s_done;
static volatile bool s_failed;
static uint32_t s_active_workers;

/* ── Worker entry ───────────────────────────────────────── */
static void stress_worker_entry(void *arg)
{
	struct stress_worker *me = (struct stress_worker *)arg;
	uint32_t total = me->total_rounds;

	for (uint32_t i = 0; i < total; i++) {
		schedule_timeout(1);
		module_atomic_add_fetch((volatile uint64_t *)&me->rounds_done, 1);
	}

	kprintf("[sched_stress] %s CPU%u: %u/%u done\n",
		me->name, me->cpu, total, total);
	__atomic_store_n(&me->finished, true, __ATOMIC_RELEASE);
}

/* ── selftest API ───────────────────────────────────────── */

static void stress_configure(const char *key, const char *value)
{
	uint32_t v = 0;
	for (const char *c = value; *c >= '0' && *c <= '9'; c++)
		v = v * 10 + (uint32_t)(*c - '0');

	if (strcmp(key, "rounds") == 0) {
		if (v == 0) v = 200;
		if (v > 10000) v = 10000;
		__atomic_store_n(&s_cfg_rounds, v, __ATOMIC_RELEASE);
	} else if (strcmp(key, "workers") == 0) {
		if (v > MAX_WORKERS) v = MAX_WORKERS;
		__atomic_store_n(&s_cfg_workers, v, __ATOMIC_RELEASE);
	} else if (strcmp(key, "timeout_ticks") == 0) {
		if (v == 0) v = 5000;
		__atomic_store_n(&s_timeout_ticks, v, __ATOMIC_RELEASE);
	}
}

static int stress_prepare(void)
{
	uint32_t total_rounds = __atomic_load_n(&s_cfg_rounds,
		__ATOMIC_ACQUIRE);
	uint32_t cfg_w = __atomic_load_n(&s_cfg_workers, __ATOMIC_ACQUIRE);

	uint64_t online_cpus = __atomic_load_n(&online, __ATOMIC_ACQUIRE);
	uint32_t max_cpus = (uint32_t)(online_cpus > 0 ? online_cpus : 1);
	if (max_cpus > MAX_WORKERS) max_cpus = MAX_WORKERS;

	uint32_t w = cfg_w > 0 ? cfg_w : max_cpus;
	if (w > max_cpus) w = max_cpus;
	if (w == 0) w = 1;

	s_active_workers = w;

	kprintf("[sched_stress] prepare: %u workers × %u rounds, "
		"%llu CPUs online\n",
		w, total_rounds, online_cpus);

	for (uint32_t i = 0; i < w; i++) {
		struct stress_worker *sw = &s_workers[i];
		sw->total_rounds = total_rounds;
		sw->cpu    = i % max_cpus;
		sw->rounds_done = 0;
		sw->finished = false;
		int cpu_id = (int)sw->cpu;

		for (int j = 0; j < 7; j++)
			sw->name[j] = '\0';
		sw->name[0] = 'w';
		sw->name[1] = (char)('0' + i);

		s_worker_threads[i] = selftest_create_worker(
			cpu_id, stress_worker_entry, sw->name, sw);

		if (!s_worker_threads[i]) {
			kprintf("[sched_stress] FAIL: spawn worker %u "
				"on CPU %u failed\n", i, sw->cpu);
			for (uint32_t j = 0; j < i; j++)
				selftest_discard_worker(s_worker_threads[j]);
			__atomic_store_n(&s_failed, true,
				__ATOMIC_RELEASE);
			__atomic_store_n(&s_done, true,
				__ATOMIC_RELEASE);
			return -1;
		}
	}

	return 0;
}

static void stress_start(void)
{
	uint32_t w = s_active_workers;
	for (uint32_t i = 0; i < w; i++) {
		if (s_worker_threads[i])
			selftest_start_worker(s_worker_threads[i]);
	}
	kprintf("[sched_stress] start: %u workers dispatched\n", w);
}

static void stress_tick(void)
{
	if (__atomic_load_n(&s_done, __ATOMIC_ACQUIRE))
		return;

	uint32_t w = s_active_workers;
	uint32_t done_count = 0;
	uint64_t total_progress = 0;

	for (uint32_t i = 0; i < w; i++) {
		if (__atomic_load_n(&s_workers[i].finished,
			__ATOMIC_ACQUIRE))
			done_count++;
		total_progress += __atomic_load_n(
			&s_workers[i].rounds_done, __ATOMIC_ACQUIRE);
	}

	/* All workers finished → PASS */
	if (done_count >= w) {
		kprintf("[sched_stress] PASSED: %u workers × "
			"%llu total rounds\n",
			w, total_progress);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		return;
	}

	/* Timeout → FAIL */
	uint64_t elapsed = module_atomic_add_fetch(&s_elapsed, 1);
	uint32_t timeout = __atomic_load_n(&s_timeout_ticks,
		__ATOMIC_ACQUIRE);

	if (elapsed >= timeout) {
		kprintf("[sched_stress] FAIL: timeout (%llu ticks) — "
			"%u/%u workers done, %llu total rounds\n",
			elapsed, done_count, w, total_progress);
		__atomic_store_n(&s_failed, true, __ATOMIC_RELEASE);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		return;
	}

	/* Progress watchdog: if no progress for 300 ticks, deadlock */
	if (elapsed % 200 == 0) {
		uint64_t last = __atomic_load_n(&s_last_progress,
			__ATOMIC_ACQUIRE);
		if (total_progress == last && elapsed > 300) {
			kprintf("[sched_stress] FAIL: stalled at "
				"tick %llu — %llu total rounds, "
				"%u/%u workers done\n",
				elapsed, total_progress,
				done_count, w);
			__atomic_store_n(&s_failed, true,
				__ATOMIC_RELEASE);
			__atomic_store_n(&s_done, true,
				__ATOMIC_RELEASE);
			return;
		}
		__atomic_store_n(&s_last_progress, total_progress,
			__ATOMIC_RELEASE);
	}

	/* Progress report every 500 ticks */
	if (elapsed % 500 == 0)
		kprintf("[sched_stress] tick %llu: %llu rounds / "
			"%u workers done\n",
			elapsed, total_progress, done_count);
}

static bool stress_done(void)
{
	return __atomic_load_n(&s_done, __ATOMIC_ACQUIRE);
}

static bool stress_passed(void)
{
	return !__atomic_load_n(&s_failed, __ATOMIC_ACQUIRE);
}

/* ── Registration ───────────────────────────────────────── */
static const struct selftest sched_stress_test = {
	.name      = "sched_stress",
	.configure = stress_configure,
	.prepare   = stress_prepare,
	.start     = stress_start,
	.tick      = stress_tick,
	.done      = stress_done,
	.passed    = stress_passed,
};

int selftest_init(void)
{
	kprintf("[sched_stress] selftest_init\n");
	return selftest_register(&sched_stress_test);
}
EXPORT_SYMBOL(selftest_init);
