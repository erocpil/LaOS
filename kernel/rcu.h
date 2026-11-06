#ifndef __RCU_H__
#define __RCU_H__

/*
 * rcu.h — RCU 读侧免锁同步接口
 */
#include "config.h"
#include "lock.h"
#include "list.h"
#include "atomic.h"
#include "cpu.h"
#include "thread.h"

#if CONFIG_RCU
struct rcu_instance_metric {
	struct list_node node;
	// reader
	uint32_t n_readers;
	uint32_t readers_alive;
	uint64_t iters[MAX_CPUS];
	uint64_t iters_prev[MAX_CPUS];
	char name[16];
};

struct rcu_metric {
	// writer
	uint32_t gp_seq;
	uint32_t n_rcu;
	struct list_node head;
	// updater
	uint32_t gp_sync_times;
	uint64_t last_gp_cycles;  // 最近一次 synchronize_rcu 的耗时(cycles)
	uint64_t last_gp_end_tsc; // 最近一次 GP 结束的 TSC，用于算两次 GP 间隔
	// renderer
	uint64_t frame_no; // stats_rcu 已渲染帧数
};

extern struct rcu_metric rcu_metric;

/* 可抢占 RCU 的全局状态。
 *
 * - gp_seq：当前宽限期序号，由 synchronize_rcu() 原子递增；CPU 在安静点
 *   更新本地 rcu_gp_seq_seen。
 * - blocked_tasks：被抢占时仍位于 RCU 临界区内的线程链表；宽限期等待阶段
 *   必须等到该链表清空。
 * - blocked_lock：保护 blocked_tasks 的自旋锁。
 *
 * 字段访问规则：
 * - gp_seq 通过 atomic_* 接口访问。
 * - blocked_tasks 的增删和空链表检查均须持有 blocked_lock。
 */
typedef struct {
	atomic_t gp_seq;
	spinlock_t blocked_lock;
	struct list_node blocked_tasks;
} rcu_global_state_t;

extern rcu_global_state_t rcu_state;

/* 宽限期同步接口。 */
void synchronize_rcu(void);

/* 调度器与 RCU 的集成钩子。 */
void rcu_check_quiescent_state(void);
void rcu_note_context_switch(struct thread *prev);

/*
 * 进入 RCU 读侧临界区。
 *
 * 内联实现避免读侧调用开销，并依赖 cpu.h 提供的 cpu_get_ctx()。
 */
static inline void rcu_read_lock(void)
{
	/*
	 * nesting 属于线程，仅在该线程为本 CPU 的 current 时访问。读侧临界区允许
	 * 被抢占；调度器会将 nesting 非零的 prev 登记到 blocked_tasks。线程恢复后，
	 * nesting 值随线程保留。
	 */
	cpu_get_ctx()->current->rcu_nesting++;
}

static inline void rcu_read_unlock(void)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *t = ctx->current;
	t->rcu_nesting--;

	if (t->rcu_nesting == 0) {
		/*
		 * 若本线程在临界区内曾被抢占，rcu_note_context_switch 已将其
		 * 登记到 blocked_tasks。退出临界区时须移除该登记，
		 * 否则 synchronize_rcu 阶段二会永远等不到列表为空。
		 *
		 * 关中断 + 持锁的复合临界区，与 rcu_note_context_switch 的
		 * 两条路径均先关中断再获取同一把锁，避免与本 CPU 的
		 * timer tick / IPI 内潜在的同锁路径冲突。
		 *
		 * 此处显式调用 save_and_disable_interrupts() 与 spin_lock()，以避免
		 * 当前工具链在 spin_lock_irqsave() 内联展开时产生的 maybe-uninitialized
		 * 诊断；两种写法执行相同的中断保存和加锁操作。
		 */
		if (t->rcu_blocked) {
			uint64_t flags = save_and_disable_interrupts();
			spin_lock(&rcu_state.blocked_lock);
#if CONFIG_RCU_DEBUG
			/* 摘除前快照 */
			L("[RCU UNL] CPU %d t=%s blknode=(%p,%p) head=(%p,%p)", ctx->id, t->name,
				t->rcu_blocked_node.prev, t->rcu_blocked_node.next,
				rcu_state.blocked_tasks.prev, rcu_state.blocked_tasks.next);
#endif
			list_del(&t->rcu_blocked_node);
			spin_unlock(&rcu_state.blocked_lock);
			restore_interrupts(flags);

			t->rcu_blocked = 0;
		}
		/* 退出临界区即一次安静点，更新本 CPU 已观察到的 gp_seq */
		ctx->rcu_gp_seq_seen = atomic_read(&rcu_state.gp_seq);
	}
}

#else /* !CONFIG_RCU */

/* RCU 关闭时的 no-op stub
 *
 * 目的：保留 reader/updater/集成 三类 API 的符号与签名，让所有调用点
 *        无需 #if 包裹即可继续编译；运行时全部退化为零开销 no-op。
 *
 * rcu.c 在 CONFIG_RCU=0 时只导出空函数，因此 rcu_state 全局符号不存在；
 * 这里也不暴露 rcu_global_state_t / rcu_state 声明。
 */
struct thread; /* 前向声明：关闭时不引入 thread.h 依赖也能编译 */
static inline void rcu_read_lock(void)
{ /* no-op */
}
static inline void rcu_read_unlock(void)
{ /* no-op */
}
static inline void synchronize_rcu(void)
{ /* no-op */
}
static inline void rcu_check_quiescent_state(void)
{ /* no-op */
}
static inline void rcu_note_context_switch(struct thread *prev)
{
	(void)prev;
}

#endif /* CONFIG_RCU */

#endif
