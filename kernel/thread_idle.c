/*
 * CPU 空闲线程。
 *
 * 空闲循环先关闭中断、检查运行队列并回收僵尸线程；队列为空时执行 sti; hlt
 * 等待中断，唤醒后重新检查运行队列。
 *
 * 空队列检查与等待之间必须避免丢失唤醒。x86 的 STI 延迟机制允许
 * idle 路径在关闭中断检查队列后执行 sti; hlt；待处理中断会在 HLT
 * 后得到响应，CPU 醒来后重新检查运行队列。
 */

#include "cpu.h"
#include "sched.h"
#include "arch_irq.h"
#include "thread.h"
#include "atomic.h"
#include "debug.h"

void idle_task_function(void *arg)
{
	(void)arg;
	struct cpu_context *ctx = cpu_get_ctx();
	int cpu = ctx->id;

	while (1) {
		/* 关中断后检查运行队列并清理僵尸线程，避免检查与进入等待之间
		 * 因本地中断插入唤醒事件。 */
		arch_local_irq_disable();

		check_need_schedule();

		int n_zombies = atomic64_read(&ctx->zombiequeue.count);
		while (n_zombies--) {
			struct thread *t = cpu_dequeue_zombie_tail(cpu);
			if (!t) {
				break;
			}
			thread_destroy(t);
			L("zombie thread %p %s %ld @ %d destroyed", t, t->name, t->id, cpu);
		}

		/* 有可运行线程或待处理调度请求时，开放 IRQ 后主动调度。 */
		if (atomic64_read(&ctx->runqueue.count) > 0 || check_need_schedule()) {
			arch_local_irq_enable();
			schedule();
			continue;
		}

		/* 当前 IF 为 0；STI 的中断延迟窗口覆盖紧随其后的 HLT。 */
		arch_cpu_safe_halt();
	}
}

struct thread *thread_create_idle(void (*entry)(), void *data)
{
	uint64_t flags = save_and_disable_interrupts();

	struct thread *t = thread_create_common(entry, data);
	if (t) {
		t->is_idle = true;
		if (t->id) {
			panic("idle pid should be 0");
		}
	}

	restore_interrupts(flags);

	return t;
}
