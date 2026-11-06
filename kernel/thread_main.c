/*
 * 主内核线程初始化。
 * 将启动阶段的执行流纳入线程调度器管理。
 */

#include "cpu.h"
#include "gdt.h"
#include "heap.h"
#include "string.h"
#include "thread.h"
#include "debug.h"

struct thread *g_main_thread;

extern uint64_t stack_top;

void thread_init_main(void)
{
	L("main thread bootstrap");
	struct cpu_context *ctx = cpu_get_ctx();
	if (!ctx) {
		panic("main thread initialization requires a local CPU context");
	}
	g_main_thread = kmalloc(sizeof(struct thread));
	if (!g_main_thread) {
		panic("Cannot kmalloc(main thread)");
	}
	memset(g_main_thread, 0, sizeof(struct thread));
	thread_priority_init(g_main_thread);
	g_main_thread->id = THREAD_SET_KERNEL_PID();
	g_main_thread->self = g_main_thread;
	g_main_thread->target_cpu = ctx->id;
	INIT_LIST_NODE(&g_main_thread->node);
	INIT_LIST_NODE(&g_main_thread->wait_node);
	memcpy(g_main_thread->name, "main", strlen("main") + 1);
	thread_set_status(g_main_thread, THREAD_RUNNING);
	// 主线程使用内核初始栈，无需记录待释放的栈基址。
	g_main_thread->rsp = 0;
	g_main_thread->kernel_stack = (void *)ctx->kernel_stack;
	tss_set_rsp0(ctx->id, ctx->kernel_stack);
	ctx->current = g_main_thread;

	// 将主线程登记为当前执行线程。
	g_current_thread = g_main_thread;
	// 当前调度器将运行线程保留在队列中，切出后才能再次选中主线程。
	cpu_enqueue(ctx->id, g_main_thread);
	thread_set_status(g_main_thread, THREAD_RUNNING);
	cpu_assert_sched_context_ready();

	L("Main Thread Initialized!");
}
