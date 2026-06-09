/*
 * test_fpu_context.c — x86_64 FPU context save/restore selftest
 *
 * Spawns two kernel worker threads that each write a distinct
 * x87 ST(0) / XMM0 pattern, yield via schedule_timeout() to force
 * a full context-switch round-trip, then verify their FPU state was
 * correctly saved and restored by switch_to's fxsave64/fxrstor64.
 *
 * The selftest tick (called from timer ISR) is non-blocking — it
 * only polls the workers' completion state.
 *
 * XMM0 ABI note: the test captures XMM0 to a local buffer via
 * movdqu immediately after writing, then after schedule_timeout()
 * compares hardware XMM0 against that buffer.  This comparison is
 * valid because the kernel is compiled with -mno-sse and switch_to
 * saves/restores the full FXSAVE area.  If the kernel ever enables
 * SSE in its own code paths, the test would need to move the
 * write→yield→verify sequence into a self-contained asm function.
 *
 * Controlled by task.conf directive:
 *   @test fpu_context module=test_fpu_context.mo rounds=500
 */

#include "selftest.h"
#include "thread.h"
#include "sched.h"
#include "printf.h"
#include "string.h"
#include "export.h"

/* ── Worker patterns ──────────────────────────────────── */

/* Worker A: XMM pattern */
static const uint8_t s_pattern_a[16] __attribute__((aligned(16))) = {
	0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
};

/* Worker B: distinct pattern */
static const uint8_t s_pattern_b[16] __attribute__((aligned(16))) = {
	0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
	0xBE, 0xBA, 0xFE, 0xCA, 0xEF, 0xBE, 0xAD, 0xDE,
};

/* Worker state — owned by one worker thread. */
struct fpu_worker {
	volatile uint32_t rounds_done;
	volatile bool     failed;
	volatile bool     done;
	uint32_t          total_rounds;
	const uint8_t    *xmm_pattern;
	double            x87_expected;
	const char       *name;
};

static struct fpu_worker s_workers[2];
static struct thread *s_worker_threads[2];

/* ── FPU helpers (inline asm — modules are -mno-sse) ──── */

/* Uses movdqu (unaligned) because the compiler may place the "m"
 * operand at a non-16-byte-aligned stack slot. */
static void xmm_write_pattern(const uint8_t pattern[16])
{
	__asm__ volatile("movdqu %0, %%xmm0" :: "m"(*pattern) : "memory");
}

static void xmm_read(uint8_t buf[16])
{
	__asm__ volatile("movdqu %%xmm0, %0" : "=m"(*buf) :: "memory");
}

static void x87_write(double val)
{
	__asm__ volatile("fldl %0" :: "m"(val) : "memory");
}

static int x87_check(double expected)
{
	double val;
	__asm__ volatile("fstpl %0" : "=m"(val) :: "memory");
	return val == expected;
}

/* ── Worker thread entry ──────────────────────────────── */

static void fpu_worker_entry(void *arg)
{
	struct fpu_worker *me = (struct fpu_worker *)arg;
	uint8_t saved_xmm[16] __attribute__((aligned(16)));

	kprintf("[fpu_ctx] %s worker started, %u rounds\n",
		me->name, me->total_rounds);

	for (uint32_t round = 0; round < me->total_rounds; round++) {
		x87_write(me->x87_expected);
		xmm_write_pattern(me->xmm_pattern);

		/* Capture XMM0 ground truth before the yield. */
		xmm_read(saved_xmm);

		/* Yield to force context-switch round-trip. */
		schedule_timeout(1);

		/* Verify x87 ST(0) survived. */
		if (!x87_check(me->x87_expected)) {
			kprintf("[fpu_ctx] %s FAIL: x87 round %u\n",
				me->name, round);
			__atomic_store_n(&me->failed, true,
				__ATOMIC_RELEASE);
			break;
		}

		/* Verify XMM0 survived. */
		{
			uint8_t cur[16] __attribute__((aligned(16)));
			xmm_read(cur);
			if (memcmp(cur, saved_xmm, 16) != 0) {
				kprintf("[fpu_ctx] %s FAIL: XMM0 "
					"round %u\n", me->name, round);
				__atomic_store_n(&me->failed, true,
					__ATOMIC_RELEASE);
				break;
			}
		}

		__atomic_store_n(&me->rounds_done, round + 1,
			__ATOMIC_RELEASE);

		if ((round + 1) % 100 == 0)
			kprintf("[fpu_ctx] %s: %u/%u OK\n",
				me->name, round + 1, me->total_rounds);
	}

	__atomic_store_n(&me->done, true, __ATOMIC_RELEASE);
	kprintf("[fpu_ctx] %s done (failed=%d)\n",
		me->name, (int)__atomic_load_n(&me->failed,
		__ATOMIC_ACQUIRE));
}

/* ── selftest API ──────────────────────────────────────── */

static volatile uint32_t s_cfg_rounds = 500;

static void fpu_configure(const char *key, const char *value)
{
	if (strcmp(key, "rounds") == 0) {
		uint32_t r = 0;
		for (const char *c = value; *c >= '0' && *c <= '9'; c++)
			r = r * 10 + (uint32_t)(*c - '0');
		if (r == 0) r = 500;
		__atomic_store_n(&s_cfg_rounds, r, __ATOMIC_RELEASE);
	}
}

static int fpu_prepare(void)
{
	uint32_t total = __atomic_load_n(&s_cfg_rounds, __ATOMIC_ACQUIRE);

	/* Worker A: ST(0) = π */
	s_workers[0].total_rounds = total;
	s_workers[0].xmm_pattern   = s_pattern_a;
	s_workers[0].x87_expected  = 3.14159265358979323846;
	s_workers[0].name          = "A";
	s_workers[0].rounds_done   = 0;
	s_workers[0].failed        = false;
	s_workers[0].done          = false;

	/* Worker B: ST(0) = 1.0, XMM = reversed pattern */
	s_workers[1].total_rounds = total;
	s_workers[1].xmm_pattern   = s_pattern_b;
	s_workers[1].x87_expected  = 1.0;
	s_workers[1].name          = "B";
	s_workers[1].rounds_done   = 0;
	s_workers[1].failed        = false;
	s_workers[1].done          = false;

	kprintf("[fpu_ctx] prepare: %u rounds, creating 2 workers\n", total);

	/* Create both workers first; neither is runnable until start(). */
	s_worker_threads[0] = selftest_create_worker(0, fpu_worker_entry,
		"fpuA", &s_workers[0]);
	s_worker_threads[1] = selftest_create_worker(0, fpu_worker_entry,
		"fpuB", &s_workers[1]);

	if (!s_worker_threads[0] || !s_worker_threads[1]) {
		kprintf("[fpu_ctx] FAIL: spawn failed (A=%p B=%p)\n",
			(void *)s_worker_threads[0], (void *)s_worker_threads[1]);
		selftest_discard_worker(s_worker_threads[0]);
		selftest_discard_worker(s_worker_threads[1]);
		s_worker_threads[0] = NULL;
		s_worker_threads[1] = NULL;
		__atomic_store_n(&s_workers[0].done, true,
			__ATOMIC_RELEASE);
		__atomic_store_n(&s_workers[0].failed, true,
			__ATOMIC_RELEASE);
		__atomic_store_n(&s_workers[1].done, true,
			__ATOMIC_RELEASE);
		__atomic_store_n(&s_workers[1].failed, true,
			__ATOMIC_RELEASE);
		return -1;
	}

	return 0;
}

static void fpu_start(void)
{
	if (!s_worker_threads[0] || !s_worker_threads[1])
		return;

	kprintf("[fpu_ctx] start: workers ready\n");
	selftest_start_worker(s_worker_threads[0]);
	selftest_start_worker(s_worker_threads[1]);
}

/* Non-blocking: called from timer ISR. */
static void fpu_tick(void)
{
}

static bool fpu_done(void)
{
	return __atomic_load_n(&s_workers[0].done, __ATOMIC_ACQUIRE) &&
	       __atomic_load_n(&s_workers[1].done, __ATOMIC_ACQUIRE);
}

static bool fpu_passed(void)
{
	return !__atomic_load_n(&s_workers[0].failed, __ATOMIC_ACQUIRE) &&
	       !__atomic_load_n(&s_workers[1].failed, __ATOMIC_ACQUIRE);
}

/* ── Registration ──────────────────────────────────────── */

static const struct selftest fpu_test = {
	.name      = "fpu_context",
	.configure = fpu_configure,
	.prepare   = fpu_prepare,
	.start     = fpu_start,
	.tick      = fpu_tick,
	.done      = fpu_done,
	.passed    = fpu_passed,
};

int selftest_init(void)
{
	kprintf("[fpu_ctx] selftest_init\n");
	return selftest_register(&fpu_test);
}
EXPORT_SYMBOL(selftest_init);
