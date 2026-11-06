/*
 * test_fpu_context.c — x86_64 FPU 上下文保存与恢复自测。
 *
 * 创建两个内核工作线程，分别设置不同的 x87 ST(0) 和 XMM0 状态，并通过
 * schedule_timeout() 触发上下文切换；恢复运行后检查 switch_to() 是否正确
 * 保存并恢复 FPU 状态。
 *
 * 定时器中断调用的自测 tick 不阻塞，只检查工作线程的完成状态。
 *
 * XMM0 检查通过 movdqu 在写入后立即保存参考值，并在 schedule_timeout() 返回后
 * 与硬件寄存器比较。当前内核使用 -mno-sse 编译，且 switch_to() 保存、恢复完整的
 * FXSAVE 区域，因此该比较不会被内核自身的 SSE 指令干扰。若内核代码启用 SSE，
 * 写入、让出和验证步骤须移入独立汇编函数。
 *
 * 由 task.conf 指令配置：
 *   @test fpu_context module=test_fpu_context.mo rounds=500
 */

#include "tests/selftest.h"
#include "thread.h"
#include "sched.h"
#include "printf.h"
#include "string.h"
#include "export.h"

/* 工作线程使用的寄存器状态模式。 */

/* 工作线程 A 的 XMM 状态模式。 */
static const uint8_t s_pattern_a[16] __attribute__((aligned(16))) = {
	0xDE,
	0xAD,
	0xBE,
	0xEF,
	0xCA,
	0xFE,
	0xBA,
	0xBE,
	0x00,
	0x11,
	0x22,
	0x33,
	0x44,
	0x55,
	0x66,
	0x77,
};

/* 工作线程 B 使用不同的寄存器状态模式。 */
static const uint8_t s_pattern_b[16] __attribute__((aligned(16))) = {
	0x77,
	0x66,
	0x55,
	0x44,
	0x33,
	0x22,
	0x11,
	0x00,
	0xBE,
	0xBA,
	0xFE,
	0xCA,
	0xEF,
	0xBE,
	0xAD,
	0xDE,
};

/* 每个工作线程独立拥有的测试状态。 */
struct fpu_worker {
	volatile uint32_t rounds_done;
	volatile bool failed;
	volatile bool done;
	uint32_t total_rounds;
	const uint8_t *xmm_pattern;
	double x87_expected;
	const char *name;
};

static struct fpu_worker s_workers[2];
static struct thread *s_worker_threads[2];

/* FPU 辅助函数；模块以 -mno-sse 编译，寄存器操作由内联汇编完成。 */

/* 使用允许非对齐地址的 movdqu，因为编译器不保证栈操作数按 16 字节对齐。 */
static void xmm_write_pattern(const uint8_t pattern[16])
{
	__asm__ volatile("movdqu %0, %%xmm0" ::"m"(*pattern) : "memory");
}

static void xmm_read(uint8_t buf[16])
{
	__asm__ volatile("movdqu %%xmm0, %0" : "=m"(*buf)::"memory");
}

static void x87_write(double val)
{
	__asm__ volatile("fldl %0" ::"m"(val) : "memory");
}

static int x87_check(double expected)
{
	double val;
	__asm__ volatile("fstpl %0" : "=m"(val)::"memory");
	return val == expected;
}

/* 工作线程入口。 */

static void fpu_worker_entry(void *arg)
{
	struct fpu_worker *me = (struct fpu_worker *)arg;
	uint8_t saved_xmm[16] __attribute__((aligned(16)));

	kprintf("[fpu_ctx] %s worker started, %u rounds\n", me->name, me->total_rounds);

	for (uint32_t round = 0; round < me->total_rounds; round++) {
		x87_write(me->x87_expected);
		xmm_write_pattern(me->xmm_pattern);

		/* 让出前保存 XMM0 的参考值。 */
		xmm_read(saved_xmm);

		/* 通过让出触发上下文切换。 */
		schedule_timeout(1);

		/* 确认 x87 ST(0) 状态得到保留。 */
		if (!x87_check(me->x87_expected)) {
			kprintf("[fpu_ctx] %s FAIL: x87 round %u\n", me->name, round);
			__atomic_store_n(&me->failed, true, __ATOMIC_RELEASE);
			break;
		}

		/* 确认 XMM0 状态得到保留。 */
		{
			uint8_t cur[16] __attribute__((aligned(16)));
			xmm_read(cur);
			if (memcmp(cur, saved_xmm, 16) != 0) {
				kprintf("[fpu_ctx] %s FAIL: XMM0 "
					"round %u\n",
					me->name, round);
				__atomic_store_n(&me->failed, true, __ATOMIC_RELEASE);
				break;
			}
		}

		__atomic_store_n(&me->rounds_done, round + 1, __ATOMIC_RELEASE);

		if ((round + 1) % 100 == 0) {
			kprintf("[fpu_ctx] %s: %u/%u OK\n", me->name, round + 1, me->total_rounds);
		}
	}

	__atomic_store_n(&me->done, true, __ATOMIC_RELEASE);
	kprintf("[fpu_ctx] %s done (failed=%d)\n", me->name,
		(int)__atomic_load_n(&me->failed, __ATOMIC_ACQUIRE));
}

/* 自测接口。 */

static volatile uint32_t s_cfg_rounds = 500;

static void fpu_configure(const char *key, const char *value)
{
	if (strcmp(key, "rounds") == 0) {
		uint32_t r = 0;
		for (const char *c = value; *c >= '0' && *c <= '9'; c++) {
			r = r * 10 + (uint32_t)(*c - '0');
		}
		if (r == 0) {
			r = 500;
		}
		__atomic_store_n(&s_cfg_rounds, r, __ATOMIC_RELEASE);
	}
}

static int fpu_prepare(void)
{
	uint32_t total = __atomic_load_n(&s_cfg_rounds, __ATOMIC_ACQUIRE);

	/* 工作线程 A：ST(0) 设置为 π。 */
	s_workers[0].total_rounds = total;
	s_workers[0].xmm_pattern = s_pattern_a;
	s_workers[0].x87_expected = 3.14159265358979323846;
	s_workers[0].name = "A";
	s_workers[0].rounds_done = 0;
	s_workers[0].failed = false;
	s_workers[0].done = false;

	/* 工作线程 B：ST(0) 设置为 1.0，XMM0 使用反向模式。 */
	s_workers[1].total_rounds = total;
	s_workers[1].xmm_pattern = s_pattern_b;
	s_workers[1].x87_expected = 1.0;
	s_workers[1].name = "B";
	s_workers[1].rounds_done = 0;
	s_workers[1].failed = false;
	s_workers[1].done = false;

	kprintf("[fpu_ctx] prepare: %u rounds, creating 2 workers\n", total);

	/* 先创建两个工作线程；调用 start() 前它们不会进入可运行状态。 */
	s_worker_threads[0] = selftest_create_worker(0, fpu_worker_entry, "fpuA", &s_workers[0]);
	s_worker_threads[1] = selftest_create_worker(0, fpu_worker_entry, "fpuB", &s_workers[1]);

	if (!s_worker_threads[0] || !s_worker_threads[1]) {
		kprintf("[fpu_ctx] FAIL: spawn failed (A=%p B=%p)\n", (void *)s_worker_threads[0],
			(void *)s_worker_threads[1]);
		selftest_discard_worker(s_worker_threads[0]);
		selftest_discard_worker(s_worker_threads[1]);
		s_worker_threads[0] = NULL;
		s_worker_threads[1] = NULL;
		__atomic_store_n(&s_workers[0].done, true, __ATOMIC_RELEASE);
		__atomic_store_n(&s_workers[0].failed, true, __ATOMIC_RELEASE);
		__atomic_store_n(&s_workers[1].done, true, __ATOMIC_RELEASE);
		__atomic_store_n(&s_workers[1].failed, true, __ATOMIC_RELEASE);
		return -1;
	}

	return 0;
}

static void fpu_start(void)
{
	if (!s_worker_threads[0] || !s_worker_threads[1]) {
		return;
	}

	kprintf("[fpu_ctx] start: workers ready\n");
	selftest_start_worker(s_worker_threads[0]);
	selftest_start_worker(s_worker_threads[1]);
}

/* 由定时器中断调用，不得阻塞。 */
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

/* 注册自测。 */

static const struct selftest fpu_test = {
	.name = "fpu_context",
	.configure = fpu_configure,
	.prepare = fpu_prepare,
	.start = fpu_start,
	.tick = fpu_tick,
	.done = fpu_done,
	.passed = fpu_passed,
};

int selftest_init(void)
{
	kprintf("[fpu_ctx] selftest_init\n");
	return selftest_register(&fpu_test);
}
EXPORT_SYMBOL(selftest_init);
