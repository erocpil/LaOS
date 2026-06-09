/*
 * rcu.c - 读侧免锁的 RCU 同步
 *
 * 方案 B:可抢占 RCU.读侧 rcu_read_lock 仅递增嵌套计数，
 * 允许临界区被抢占；被抢占线程由 blocked_tasks 追踪。
 */
#include "rcu.h"

#if CONFIG_RCU

#include "cpu.h"
#include "arch_dispatch.h"
#include "list.h"
#include "lock.h"
#include "sched.h"
#include "atomic.h"
#include "thread.h"

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

/**
 * rcu_check_quiescent_state()
 *
 * 调用上下文:timer tick 中断处理程序，每个 CPU 各自调用。
 * 作用：本 CPU 不在 RCU 临界区时，把全局 gp_seq 复制到 per-CPU
 *   rcu_gp_seq_seen:这是一次"安静点"的记账。
 *
 * 若 nesting > 0，说明 tick 打断了一段正在执行的 RCU 临界区，
 * 此刻不能算安静点。后续两条路径之一会兜底：
 *   - 临界区正常退出:rcu_read_unlock 更新 gp_seq_seen
 *   - 临界区中被抢占:rcu_note_context_switch 把线程登记进
 *     blocked_tasks，最终由 rcu_read_unlock 摘除并记账
 */
void rcu_check_quiescent_state(void)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *cur = ctx->current;

	/* 在线程子系统初始化之前，current 可能为 NULL (ARM64 早期启动) */
	if (!cur)
		return;

	/*
	 * 读 current->rcu_nesting:tick 打中本 CPU 时，current 必为
	 * 当前正在运行的线程。该字段仅本 CPU 自己写，本路径只读，无并发。
	 */
	if (cur->rcu_nesting == 0) {
		ctx->rcu_gp_seq_seen = atomic_read(&rcu_state.gp_seq);
	}
}

/**
 * synchronize_rcu() - 阻塞调用方直到当前所有 reader 退出临界区。
 *
 * 两阶段实现：
 *   阶段一：等所有 CPU 都至少经过一次 quiescent state(即 gp_seq_seen 追上本次 target).
 *   阶段二：等"被抢占且仍在临界区内"的所有任务自己退出(blocked_tasks 清空).
 *
 * 阶段顺序解释：
 * - 阶段一只能保证 CPU 经过过一次 nesting==0 的瞬间，但不能保证
 *    那一刻没有线程被抢占着卡在临界区中，这部分由阶段二兜底。
 * - 反过来，阶段二独自不够，因为有些 CPU 可能从未发生过抢占，但
 *    确实在跑临界区，必须靠阶段一等到 nesting 自然降到 0.
 *
 * 时间特征：
 * - 宽限期下界约为 1 个 tick x cpu_count(最坏情况每核心都要等一个
 *    新 tick 才能记账).LaOS 默认 100Hz，每核心最长 10ms.
 * - 当前未实现 stall detector:若某 CPU 长时间不发生 tick(例如
 *    一直处于 NMI 或外设硬挂)，等待方将无限自旋调用 schedule()。
 *    FIXME:参考 Linux RCU 的 force_quiescent_state 机制后补。
 *
 * 锁与中断：
 * - 阶段二循环外不持任何锁，schedule() 调用合法(__schedule 路径
 *   要求本地中断关闭且不持 spinlock)。
 * - 阶段二窗口内的 spin_lock 不关中断；若调用线程在持锁时被 timer 打断并随即被
 *   切走，锁会被多保持一段时间，其他 CPU 上的 rcu_note_context_switch 自旋等待，
 *   不死锁，但延迟劣化。教学场景接受此代价。
 */
void synchronize_rcu(void)
{
	/*
	 * atomic_inc 返回加后值(__atomic_add_fetch ACQ_REL),
	 * target 即新一代宽限期序号。
	 */
	int target = atomic_inc(&rcu_state.gp_seq);

	uint64_t t0 = rdtsc();
	rcu_metric.gp_seq = target;
	rcu_metric.gp_sync_times++;

	/* 阶段一：等所有 CPU 都确认过 target */
	for (int cpu = 0; cpu < (int)g_cpu_count; cpu++) {
		struct cpu_context *c = g_cpu_contexts[cpu];
		if (!c) continue;
		while (c->rcu_gp_seq_seen < target) {
			schedule();
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
	}

	uint64_t t1 = rdtsc();
	rcu_metric.last_gp_cycles = t1 - t0;
	rcu_metric.last_gp_end_tsc = t1;
}

/**
 * rcu_note_context_switch()
 *
 * 调用上下文:__schedule 切换 prev -> next 之前。
 * 调用前提：本 CPU 中断已关（由 __schedule 入口保证）。
 *
 * prev 当前是否在 RCU 临界区？
 * - 是(nesting > 0)：登记进全局 blocked_tasks.synchronize_rcu
 *    阶段二会等 prev 真正退出临界区时(rcu_read_unlock 内)摘除。
 * - 否(nesting == 0)：本次切走即一次安静点，更新本 CPU 的 gp_seq_seen。
 *
 * 锁与中断：
 * - 入口已关闭本地中断，本 CPU 不会被 IRQ 抢占；用裸 spin_lock 即可，
 *   无需 spin_lock_irqsave。
 * - 与 rcu_read_unlock 路径锁的是同一把 blocked_lock。两条路径
 *   都先关中断，后拿锁，锁序一致，不会死锁。
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
				ctx->id, prev->name, prev->rcu_nesting, prev->rcu_blocked,
				prev->node.prev, prev->node.next,
				prev->rcu_blocked_node.prev, prev->rcu_blocked_node.next);
#endif

		spin_lock(&rcu_state.blocked_lock);
		if (!prev->rcu_blocked) {
			prev->rcu_blocked = 1;
			/* 把 prev 自身的节点加入到全局 blocked_tasks 头部。 */
			list_add(&prev->rcu_blocked_node, &rcu_state.blocked_tasks);

			/* list_add 后再快照，看链表是否串成功 */
#if CONFIG_RCU_DEBUG
			L("[RCU NCS+] CPU %d prev=%s blknode=(%p,%p) head=(%p,%p)",
					ctx->id, prev->name,
					prev->rcu_blocked_node.prev, prev->rcu_blocked_node.next,
					rcu_state.blocked_tasks.prev, rcu_state.blocked_tasks.next);
#endif
		}
		spin_unlock(&rcu_state.blocked_lock);
	} else {
		ctx->rcu_gp_seq_seen = atomic_read(&rcu_state.gp_seq);
	}
}

#endif
