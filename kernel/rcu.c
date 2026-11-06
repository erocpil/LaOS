/*
 * rcu.c — RCU 读侧免锁同步
 *
 * 读侧临界区允许被抢占。RCU 通过 blocked_tasks 跟踪被抢占时仍处于临界区的线程。
 */
#include "rcu.h"
#include "export.h"
#include "debug.h"

#if CONFIG_RCU

#include "cpu.h"
#include "arch_dispatch.h"
#include "list.h"
#include "lock.h"
#include "sched.h"
#include "atomic.h"
#include "thread.h"
#include "timer.h"

#define RCU_STALL_REPORT_TICKS (5 * TIMER_HZ)

static int rcu_stall_report_due(uint64_t *last_report)
{
	uint64_t now = atomic64_read(&timer_ticks);
	if (now - *last_report < RCU_STALL_REPORT_TICKS) {
		return 0;
	}
	*last_report = now;
	return 1;
}

/* rcu_state - 全局 RCU 状态实例
 *
 * blocked_lock 用 SPINLOCK_INIT() 静态初始化，等价于运行期 spin_lock_init.
 * blocked_tasks 用 LIST_NODE_INIT 自指初始化(list.h 头/节点同型设计).
 */
rcu_global_state_t rcu_state = {
	.gp_seq = ATOMIC_INIT(0),
	.blocked_lock = SPINLOCK_INIT(),
	.blocked_tasks = LIST_NODE_INIT(rcu_state.blocked_tasks),
};

struct rcu_metric rcu_metric = {
	.gp_seq = 0,
	.n_rcu = 0,
	.head = LIST_NODE_INIT(rcu_metric.head),
};

/*
 * rcu_check_quiescent_state()
 *
 * 调用上下文：每个 CPU 的 timer tick 中断处理程序。
 * 若本 CPU 当前线程不在 RCU 临界区，将全局 gp_seq 记录到本地
 * rcu_gp_seq_seen，表示本 CPU 已经过一个安静点。
 *
 * 若 nesting 大于零，本次 tick 不构成安静点。该线程退出临界区时，
 * rcu_read_unlock 更新 gp_seq_seen；若线程在临界区内被抢占，
 * rcu_note_context_switch 会将其登记到 blocked_tasks，退出时再移除。
 */
void rcu_check_quiescent_state(void)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *cur = ctx->current;

	/* 线程子系统初始化前，current 可能为空。 */
	if (!cur) {
		return;
	}

	/*
	 * timer tick 在本 CPU 上执行时，current 指向当前线程。该字段仅由本 CPU
	 * 更新；本处理路径只读取该字段。
	 */
	if (cur->rcu_nesting == 0) {
		ctx->rcu_gp_seq_seen = atomic_read(&rcu_state.gp_seq);
	}
}

/*
 * 阻塞调用方，直至本次宽限期覆盖的所有读侧临界区退出。
 *
 * 两阶段实现：
 *   阶段一等待所有 CPU 报告本次宽限期序号；阶段二等待宽限期开始时仍处于
 *   临界区、且随后被抢占的线程退出临界区并从 blocked_tasks 移除。
 *
 * 阶段一覆盖未发生上下文切换的 CPU；阶段二覆盖被抢占后仍停留在读侧临界区
 * 的线程。两阶段共同保证宽限期开始前的读者均已退出。
 *
 * 每个 CPU 最迟在下一次 tick 更新本地序号。等待期间每 5 秒报告未响应的
 * CPU 或仍在读侧临界区中的线程；诊断不会代替实际的宽限期等待。
 *
 * 阶段二等待循环不持锁，因此可以调用 schedule()。链表锁在短临界区内获取且
 * 不屏蔽中断；持锁线程若被抢占，其他 CPU 可能短暂自旋等待该锁释放。
 */
void synchronize_rcu(void)
{
	/*
	 * atomic_inc 返回递增后的值（ACQ_REL）；该值作为本次宽限期序号。
	 */
	int target = atomic_inc(&rcu_state.gp_seq);

	uint64_t t0 = rdtsc();
	uint64_t last_report = atomic64_read(&timer_ticks);
	rcu_metric.gp_seq = target;
	rcu_metric.gp_sync_times++;

	/* 阶段一：等所有 CPU 都确认过 target */
	for (int cpu = 0; cpu < (int)g_cpu_count; cpu++) {
		struct cpu_context *c = g_cpu_contexts[cpu];
		if (!c) {
			continue;
		}
		while (c->rcu_gp_seq_seen < target) {
			schedule();
			if (rcu_stall_report_due(&last_report)) {
				L("[RCU] grace period %d waiting for CPU %d: observed=%d", target,
					cpu, c->rcu_gp_seq_seen);
			}
		}
	}

	/* 阶段二：等被切走且仍在临界区的任务自己退出 */
	while (1) {
		spin_lock(&rcu_state.blocked_lock);
		int empty = list_empty(&rcu_state.blocked_tasks);
		spin_unlock(&rcu_state.blocked_lock);
		if (empty) {
			break;
		}

		schedule();
		if (rcu_stall_report_due(&last_report)) {
			uint32_t blocked_count = 0;
			spin_lock(&rcu_state.blocked_lock);
			struct list_node *pos;
			list_for_each(pos, &rcu_state.blocked_tasks) {
				blocked_count++;
			}
			spin_unlock(&rcu_state.blocked_lock);
			L("[RCU] grace period %d waiting for %u blocked reader(s)", target,
				blocked_count);
		}
	}

	uint64_t t1 = rdtsc();
	rcu_metric.last_gp_cycles = t1 - t0;
	rcu_metric.last_gp_end_tsc = t1;
}

/*
 * rcu_note_context_switch()
 *
 * 调用上下文:__schedule 切换 prev -> next 之前。
 * 调用前提：本 CPU 中断已关（由 __schedule 入口保证）。
 *
 * prev 当前是否在 RCU 临界区？
 * - nesting 大于零：将 prev 登记到 blocked_tasks；其退出临界区时由
 *   rcu_read_unlock() 移除，synchronize_rcu() 的第二阶段会等待该操作完成。
 * - nesting 为零：将本次切换记为安静点，并更新本 CPU 的 gp_seq_seen。
 *
 * 锁与中断：
 * - 入口已关闭本地中断，本 CPU 不会被 IRQ 抢占；用裸 spin_lock 即可，
 *   无需 spin_lock_irqsave。
 * - 与 rcu_read_unlock 路径锁的是同一把 blocked_lock。两条路径
 *   两条路径均先关闭中断，再获取该锁，顺序一致。
 */
void rcu_note_context_switch(struct thread *prev)
{
	struct cpu_context *ctx = cpu_get_ctx();

	/*
	 * 读 prev->rcu_nesting：__schedule 此刻本 CPU 中断已关且尚未
	 * 切走 prev，prev 仍归本 CPU 独占，无并发。
	 */
	if (prev->rcu_nesting > 0) {
		/* 进 blocked 路径前快照 */
#if CONFIG_RCU_DEBUG
		L("[RCU NCS] CPU %d prev=%s nest=%d blkd=%d "
		  "rqnode=(%p,%p) blknode=(%p,%p)",
			ctx->id, prev->name, prev->rcu_nesting, prev->rcu_blocked, prev->node.prev,
			prev->node.next, prev->rcu_blocked_node.prev, prev->rcu_blocked_node.next);
#endif

		spin_lock(&rcu_state.blocked_lock);
		if (!prev->rcu_blocked) {
			prev->rcu_blocked = 1;
			/* 把 prev 自身的节点加入到全局 blocked_tasks 头部。 */
			list_add(&prev->rcu_blocked_node, &rcu_state.blocked_tasks);

			/* list_add 后再快照，看链表是否串成功 */
#if CONFIG_RCU_DEBUG
			L("[RCU NCS+] CPU %d prev=%s blknode=(%p,%p) head=(%p,%p)", ctx->id,
				prev->name, prev->rcu_blocked_node.prev,
				prev->rcu_blocked_node.next, rcu_state.blocked_tasks.prev,
				rcu_state.blocked_tasks.next);
#endif
		}
		spin_unlock(&rcu_state.blocked_lock);
	} else {
		ctx->rcu_gp_seq_seen = atomic_read(&rcu_state.gp_seq);
	}
}

#endif

#if CONFIG_RCU
EXPORT_SYMBOL(synchronize_rcu);
EXPORT_SYMBOL(rcu_state);
EXPORT_SYMBOL(rcu_metric);
#endif
