/*
 * 主内核线程初始化。
 * 把 boot 阶段的执行流包装为一个 struct thread。
 */

#include "cpu.h"
#include "gdt.h"
#include "heap.h"
#include "string.h"
#include "thread.h"
#include "debug.h"

struct thread *g_main_thread;

extern uint64_t stack_top;

void thread_init_main()
{
	L("main thread bootstrap");
	g_main_thread = kmalloc(sizeof(struct thread));
	if (!g_main_thread) {
		panic("Cannot kmalloc(main thread)");
	}
	memset(g_main_thread, 0, sizeof(struct thread));
	thread_priority_init(g_main_thread);
	g_main_thread->id = THREAD_SET_KERNEL_PID();
	memcpy(g_main_thread->name, "main", strlen("main") + 1);
	thread_set_status(g_main_thread, THREAD_RUNNING);
	// main 线程使用内核初始栈，不需要记录基址用于释放
	g_main_thread->rsp = 0;
	// 必须手动指向自己，偏移 8 才有意义
	g_main_thread->self = g_main_thread;
	struct cpu_context *ctx = cpu_get_ctx();

	g_main_thread->kernel_stack = (void*)ctx->kernel_stack;
	tss_set_rsp0(ctx->id, ctx->kernel_stack);

	g_main_thread->target_cpu = ctx->id;
	ctx->current = g_main_thread;

	// 设为当前执行线程
	g_current_thread = g_main_thread;
	cpu_enqueue(0, g_main_thread);

	L("Main Thread Initialized!");
}
