#ifndef __ARCH_X86_H__
#define __ARCH_X86_H__

#include "debug.h"

#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif

/* 内存屏障 / cpu_relax 已迁至 arch_barrier.h */

/* -------------------------------------------------------------
 * 依赖的底层原语(需在内核中已实现)
 * ------------------------------------------------------------- */

/* 获取当前 RIP(调用点的返回地址，即 WARN_ON 所在位置)*/
#define _RET_ADDR  __builtin_return_address(0)

/* 获取当前栈帧指针，用于 backtrace */
#define _FRAME_ADDR  __builtin_frame_address(0)

/* -------------------------------------------------------------
 * dump_stack() - 打印调用栈
 * 需要内核提供栈回溯实现，接口声明如下：
 * ------------------------------------------------------------- */
void dump_stack(void);   /* 在 debug.c 中实现，见下方说明 */

/* -------------------------------------------------------------
 * panic() - halt 整个系统
 * 内核应已有此实现(从 debug.h 引入)
 * 最简实现：
 *   __attribute__((noreturn)) void panic(const char *fmt, ...) {
 *       // 打印，关中断，hlt
 *   }
 * ------------------------------------------------------------- */

/* -------------------------------------------------------------
 * warn_on_internal() - WARN_ON 的实际执行体(非内联，减少代码膨胀)
 * ------------------------------------------------------------- */
	__attribute__((noinline, cold))
static void __warn_on_internal(const char *expr_str, const char *file,
		int line, const char *func)
{
	/*
	 * 关本核中断：防止 warn 打印过程中被中断打断导致输出混乱。
	 * 注意：不用 spin_lock，因为 WARN_ON 本身可能在持锁路径上触发，
	 * 强行加锁会死锁。
	 */
	unsigned long flags;
	asm volatile(
			"pushfq         \n"
			"pop  %0        \n"
			"cli            \n"
			: "=r"(flags) :: "memory"
			);

	/* -- 打印 WARN 头部 -- */
	kprintf("\n");
	kprintf("------------[ WARN_ON ]------------\n");
	kprintf("WARNING: assertion failed: (%s)\n", expr_str);
	kprintf("  at %s:%d in %s()\n", file, line, func);
	kprintf("  RIP: %p\n", _RET_ADDR);

	/* -- 打印寄存器和调用栈 -- */
	// TODO
	dump_stack();

	kprintf("---[ panic halt ]---\n");

	/*
	 * 恢复中断标志后立即 panic.
	 * panic 本身会关中断并 hlt，这里恢复只是为了
	 * 让 panic 有机会做它自己的 irqsave.
	 */
	asm volatile(
			"push %0        \n"
			"popfq          \n"
			:: "r"(flags) : "memory"
			);

	panic("WARN_ON(%s) at %s:%d", expr_str, file, line);

	/* panic 是 noreturn，编译器知道不会执行到这里 */
	__builtin_unreachable();
}

static __always_inline void warn_on_internal(const char *expr_str,
		const char *file, int line, const char *func)
{
	__warn_on_internal(expr_str, file, line, func);
}

/* -------------------------------------------------------------
 * WARN_ON(cond) 宏
 *
 * 设计要点：
 * 1. unlikely()  - 提示编译器"正常路径不触发"，优化分支预测
 * 2. (void)(cond) - 确保 cond 的副作用只求值一次(防止多次求值)
 * 3. __FILE__/__LINE__/__func__ - 编译期嵌入位置信息，零运行时开销
 * 4. 触发后 panic，不返回
 * ------------------------------------------------------------- */
#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#define WARN_ON(cond)                                           \
	do {                                                        \
		if (unlikely(cond)) {                                   \
			warn_on_internal(#cond,                             \
					__FILE__,                          \
					__LINE__,                          \
					__func__);                         \
			/* warn_on_internal 内部调用 panic，不会返回 */     \
			__builtin_unreachable();                            \
		}                                                       \
	} while (0)

/*
 * 变体:WARN_ON_ONCE : 同一位置只打印一次(防日志爆炸)
 * 注意：实现是 panic halt，实际上触发一次就停机，
 * 此变体仅供参考(如果将来改为非 panic 行为时使用).
 */
#define WARN_ON_ONCE(cond)                                      \
	do {                                                        \
		static int __warned = 0;                                \
		if (unlikely((cond) && !__warned)) {                    \
			__warned = 1;                                       \
			warn_on_internal(#cond,                             \
					__FILE__,                          \
					__LINE__,                          \
					__func__);                         \
			__builtin_unreachable();                            \
		}                                                       \
	} while (0)

#endif
