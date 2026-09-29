/*
 * CPU 空闲线程。
 *
 * 经典 idle 循环：关中断 -> 检查 runqueue -> 清理 zombie ->
 * 队列空则 sti；hlt 等待中断 -> 中断唤醒后回到循环顶部。
 *
 * 竞态窗口 (arch_cpu_halt 的问题):
 *   关中断检查 runqueue 为空
 *   -> 中断到达， handler 把线程入队
 *   -> sti(中断在 sti 时被响应，但已错过 need_resched)
 *   -> hlt(CPU 休眠，等待下一次中断----可能永远不来)
 *
 * sti；hlt 原子序列消除了这个窗口:sti 执行后下一条指令(hlt)
 * 执行前 x86 保证不响应中断。中断在 hlt 后立即被响应，CPU 醒来，
 * 重新进入循环顶部，重新检查 runqueue.
 */

#include "cpu.h"
#include "sched.h"
#include "arch_irq.h"
#include "thread.h"
#include "atomic.h"
#include "debug.h"

void idle_task_function(void* arg)
{
	(void)arg;
	struct cpu_context *ctx = cpu_get_ctx();
	int cpu = ctx->id;

	while (1) {
		/*
		 * 关中断后执行检查和 zombie 清理。
		 * 必须在 cli 下判断 runqueue，确保"空队列 -> hlt"的判断是原子的----
		 * 不会在判断和 hlt 之间被中断插入一个新线程。
		 */
		arch_local_irq_disable();

		check_need_schedule();

		int n_zombies = atomic64_read(&ctx->zombiequeue.count);
		while (n_zombies--) {
			struct thread *t = cpu_dequeue_zombie_tail(cpu);
			if (!t) {
				break;
			}
			thread_destroy(t);
			L("zombie thread %p %s %ld @ %d destroyed",
					t, t->name, t->id, cpu);
		}

		/*
		 * 若有关键工作要处理，sti + schedule 让出 CPU.
		 * schedule() 内部会重新 cli，返回时 IF=0，循环顶部再次 cli 无影响。
		 */
		if (atomic64_read(&ctx->runqueue.count) > 0 || check_need_schedule()) {
			arch_local_irq_enable();
			schedule();
			continue;
		}

		/*
		 * 无事可做:sti；hlt 原子开中断并等待。
		 *
		 * 此时 IF=0(cli 在上方)，sti；hlt 保证：
		 *   - sti 置 IF=1，但下一条指令执行前不响应中断
		 *   - hlt 让 CPU 进入低功耗等待
		 *   - 任何待处理中断在 hlt 后立即唤醒 CPU
		 *
		 * 唤醒后循环回到顶部，重新 cli + 检查 runqueue.
		 */
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
