/*
 * test_ipi_delivery.c — IPI delivery stress selftest payload
 *
 * Validates that every online AP receives exactly one IPI per round,
 * with no duplicates and no missed deliveries, across configurable
 * rounds with per-round timeout.
 *
 * Controlled by task.conf directive:
 *   @test ipi_delivery module=test_ipi_delivery.mo rounds=100 timeout_ticks=1000
 *
 * Uses dedicated SELFTEST IPI vector — isolated from TLB shootdowns
 * so no cross-contamination from other tests or normal kernel operations.
 */
#include "selftest.h"
#include "ipi.h"
#include "printf.h"
#include "cpu.h"
#include "export.h"
#include "string.h"

#define IPI_MAX_CPUS 64

/* ── Test state ─────────────────────────────────────────── */
static volatile uint32_t s_rounds        = 100;
static volatile uint32_t s_timeout_ticks = 1000;
static volatile uint32_t s_stage;
static volatile uint64_t s_expected_aps;
static volatile uint32_t s_current_round;
static volatile uint64_t s_elapsed;
static volatile uint64_t s_responses[IPI_MAX_CPUS];
static volatile bool s_done;
static volatile bool s_passed;

/* ── IPI callback (runs on every AP) ────────────────────── */
static void ipi_delivery_cb(void)
{
	/* Only count during stage 1 (active delivery phase). */
	if (__atomic_load_n(&s_stage, __ATOMIC_ACQUIRE) != 1)
		return;

	struct cpu_context *ctx = cpu_get_ctx();
	if (!ctx)
		return;
	unsigned int cpu = (unsigned int)ctx->id;
	if (cpu >= IPI_MAX_CPUS)
		return;

	/* Always increment — never saturate.  Duplicate detection
	 * is done in the tick path by comparing per-CPU counts
	 * against the expected value (1 per round). */
	module_atomic_add_fetch((volatile uint64_t *)&s_responses[cpu], 1);
}

/* ── selftest API ───────────────────────────────────────── */
static void ipi_delivery_configure(const char *key, const char *value)
{
	uint32_t v = 0;
	for (const char *c = value; *c >= '0' && *c <= '9'; c++)
		v = v * 10 + (uint32_t)(*c - '0');

	if (strcmp(key, "rounds") == 0) {
		if (v == 0) v = 100;
		if (v > 10000) v = 10000;
		__atomic_store_n(&s_rounds, v, __ATOMIC_RELEASE);
	} else if (strcmp(key, "timeout_ticks") == 0) {
		if (v == 0) v = 1000;
		__atomic_store_n(&s_timeout_ticks, v, __ATOMIC_RELEASE);
	}
}

static void ipi_delivery_start(void)
{
	uint64_t total = __atomic_load_n(&online, __ATOMIC_ACQUIRE);
	s_expected_aps = (total > 0) ? total - 1 : 0;
	kprintf("[ipi_delivery] start: online=%llu expected=%llu "
		"rounds=%u timeout=%u ticks\n",
		total, s_expected_aps, s_rounds, s_timeout_ticks);
}

static void ipi_delivery_tick(void)
{
	if (__atomic_load_n(&s_done, __ATOMIC_ACQUIRE))
		return;

	uint64_t expected = s_expected_aps;
	if (!expected) {
		kprintf("[ipi_delivery] passed: single CPU\n");
		__atomic_store_n(&s_passed, true, __ATOMIC_RELEASE);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		return;
	}

	uint32_t stage = __atomic_load_n(&s_stage, __ATOMIC_ACQUIRE);

	if (stage == 0) {
		/* Init: clear per-CPU counters, register callback. */
		for (unsigned int i = 0; i < IPI_MAX_CPUS; i++)
			__atomic_store_n(&s_responses[i], 0, __ATOMIC_RELEASE);
		__atomic_store_n(&s_current_round, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&s_elapsed, 0, __ATOMIC_RELEASE);
		ipi_selftest_set_callback(ipi_delivery_cb);
		/* Release-store stage, THEN broadcast — so APs never
		 * see the IPI before they can find stage==1. */
		__atomic_store_n(&s_stage, 1, __ATOMIC_RELEASE);
		ipi_broadcast(IPI_VECTOR_SELFTEST);
		return;
	}

	/* stage 1: per-round delivery loop */
	uint32_t current = __atomic_load_n(&s_current_round, __ATOMIC_ACQUIRE);
	uint32_t rounds   = __atomic_load_n(&s_rounds, __ATOMIC_ACQUIRE);

	/* Check if all APs responded for the current round. */
	unsigned int responded = 0;
	unsigned int duplicates = 0;

	for (unsigned int cpu = 0; cpu < IPI_MAX_CPUS; cpu++) {
		uint64_t cnt = __atomic_load_n(&s_responses[cpu], __ATOMIC_ACQUIRE);
		if (cnt > 1)
			duplicates++;
		if (cnt >= 1)
			responded++;
	}

	if (duplicates > 0) {
		kprintf("[ipi_delivery] FAIL: round %u — %u CPUs responded "
			"more than once\n", current, duplicates);
		__atomic_store_n(&s_passed, false, __ATOMIC_RELEASE);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		ipi_selftest_clear_callback(ipi_delivery_cb);
		return;
	}

	if (responded >= expected) {
		/* All APs responded exactly once for this round. */
		if (current + 1 >= rounds) {
			kprintf("[ipi_delivery] passed: %u/%u rounds, "
				"%u APs per round\n",
				rounds, rounds, (unsigned int)expected);
			__atomic_store_n(&s_passed, true, __ATOMIC_RELEASE);
			__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
			ipi_selftest_clear_callback(ipi_delivery_cb);
			return;
		}
		/* Advance to next round. */
		__atomic_store_n(&s_current_round, current + 1, __ATOMIC_RELEASE);
		__atomic_store_n(&s_elapsed, 0, __ATOMIC_RELEASE);
		/* Clear per-CPU counters for the new round. */
		for (unsigned int i = 0; i < IPI_MAX_CPUS; i++)
			__atomic_store_n(&s_responses[i], 0, __ATOMIC_RELEASE);
		ipi_selftest_set_callback(ipi_delivery_cb); /* re-register */
		ipi_broadcast(IPI_VECTOR_SELFTEST);
		return;
	}

	/* Not all responded — check timeout. */
	uint64_t elapsed = ++s_elapsed;
	if (elapsed >= s_timeout_ticks) {
		kprintf("[ipi_delivery] FAIL: timeout at round %u — "
			"%u/%u APs responded\n",
			current, responded, (unsigned int)expected);
		__atomic_store_n(&s_passed, false, __ATOMIC_RELEASE);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		ipi_selftest_clear_callback(ipi_delivery_cb);
	}
}

static bool ipi_delivery_done(void)
{
	return __atomic_load_n(&s_done, __ATOMIC_ACQUIRE);
}

static bool ipi_delivery_passed(void)
{
	return __atomic_load_n(&s_passed, __ATOMIC_ACQUIRE);
}

static const struct selftest ipi_delivery_test = {
	.name      = "ipi_delivery",
	.configure = ipi_delivery_configure,
	.start     = ipi_delivery_start,
	.tick      = ipi_delivery_tick,
	.done      = ipi_delivery_done,
	.passed    = ipi_delivery_passed,
};

/* Called synchronously by kernel selftest loader */
int selftest_init(void)
{
	kprintf("[ipi_delivery] selftest_init\n");
	return selftest_register(&ipi_delivery_test);
}
EXPORT_SYMBOL(selftest_init);
