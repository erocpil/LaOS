/*
 * ist_test.c - IST 中断栈隔离自检
 *
 * 详见 ist_test.h.仅在 CONFIG_IST_TEST=1 编译入内核。
 *
 * 触发 double-fault 的原理：
 *   x86_64 遇到中断需要压 5 个寄存器(RIP/CS/RFLAGS/RSP/SS，40 字节)+
 *   error_code + int_no(我们的 ISR 存根)+ 通用寄存器保存。若当前 rsp
 *   压栈时越过合法内存边界(如页表未映射)，触发 #PF；处理 #PF 时又要
 *   压栈仍失败 -> 硬件升级为 #DF.若 #DF 依然复用当前栈继续压栈失败 ->
 *   triple-fault，CPU 重启.IST 的作用是让 #DF 立即切换到独立栈，避免
 *   触发第三次错误。
 *
 * 测试线程做法：
 *   1. 递归调用自身，每层消耗一大块栈(阻塞式，让栈快速膨胀到边界)
 *   2. 递归时通过读写触发的方式让 rsp 落到未映射区
 *   3. 期望:#DF handler 在 IST 栈上执行，能正常打印 panic 并 hlt
 *
 * 简化实现：写一个显式 far-out-of-stack 的 rsp 值让硬件立刻抛异常。
 */
#include "config.h"

#if CONFIG_IST_TEST

#include "ist_test.h"
#include "thread.h"
#include "sched.h"
#include "printf.h"
#include "debug.h"
#include "log.h"

static void ist_test_thread(void *arg)
{
	(void)arg;

	// 等 3 秒让 boot 日志输出干净，再触发
	schedule_timeout(300);

	L("[ist-test] triggering stack fault on cpu=?; expecting #DF via IST\n");

	// 方法：把 rsp 显式挪到一个不可写地址，然后主动 push.
	// 硬件遇到 push 失败 -> #PF -> #PF 处理时压栈仍失败 -> #DF.
	// 若 IST_SLOT_DF 装配正确，#DF handler 会在独立栈上运行。
	//
	// 注意：这个操作永远不会返回
	__asm__ volatile (
		"movq $0x1, %%rsp\n\t"   // rsp = 1，绝对越界
		"pushq $0\n\t"            // 触发 #PF -> #DF
		:
		:
		: "memory"
	);

	// 不会执行到这里
	L("[ist-test] UNREACHABLE - did the test fail?\n");
	for (;;) {
		__asm__ volatile ("hlt");
	}
}

void ist_test_start(void)
{
	L("[ist-test] scheduling stack-fault probe kthread\n");
	struct thread *t = thread_create(ist_test_thread, NULL);
	if (!t) {
		L("[ist-test] thread_create failed\n");
	}
}

#else /* !CONFIG_IST_TEST */

void ist_test_start(void) { /* no-op */ }

#endif
