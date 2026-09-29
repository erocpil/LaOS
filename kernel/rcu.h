#ifndef __RCU_H__
#define __RCU_H__

/*
 * rcu.h - RCU 读侧免锁同步 API
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
	uint64_t last_gp_cycles; // 最近一次 synchronize_rcu 的耗时(cycles)
	uint64_t last_gp_end_tsc; // 最近一次 GP 结束的 TSC，用于算两次 GP 间隔
							  // renderer
	uint64_t frame_no; // stats_rcu 已渲染帧数
};

extern struct rcu_metric rcu_metric;

/* rcu_global_state_t - RCU 全局状态(方案 B:可抢占 RCU)
 *
 * - gp_seq: 当前宽限期序号.synchronize_rcu 通过 atomic_inc 递增，
 *    每个 CPU 在安静点(quiescent state)将本 CPU 的
 *    rcu_gp_seq_seen 更新为最新 gp_seq.
 * - blocked_tasks: 被抢占且仍位于 RCU 临界区内的线程链表。
 *    synchronize_rcu 阶段二必须等待此链表清空。
 * - blocked_lock: 保护 blocked_tasks 链表的自旋锁。
 *
 * 字段访问规则：
 * - gp_seq 通过 atomic_* 接口访问，跨 CPU 无锁可见。
 * - blocked_tasks 的增/删/empty 检查必须持 blocked_lock.
 */
typedef struct {
	atomic_t gp_seq;
	spinlock_t blocked_lock;
	struct list_node blocked_tasks;
} rcu_global_state_t;

extern rcu_global_state_t rcu_state;

/** Updater API */
void synchronize_rcu(void);

/** 集成钩子 */
void rcu_check_quiescent_state(void);
void rcu_note_context_switch(struct thread *prev);

/**
 * rcu_read_lock() - Reader inline 实现
 *
 * 写在 header 里有两个目的：
 * 1. 让外部 .c 文件能链接到这两个符号(原本 static 在 rcu.c 里不可见)。
 * 2. 热路径零调用开销。
 *
 * 实现依赖 cpu_get_ctx()，放在 cpu.h 里。
 */
static inline void rcu_read_lock(void)
{
	/*
	 * nesting 是 per-thread 字段，仅本 CPU 上"current==该 thread"时读写。
	 * 通过 cpu_get_ctx()->current 取当前线程，这是本 CPU 上唯一活跃的
	 * 线程，无并发风险。
	 * 故意不关抢占是方案 B 与方案 A 的核心区别：
	 * 临界区允许被调度器抢占。被抢占时 rcu_note_context_switch 会
	 * 检测 prev->rcu_nesting > 0 并把 prev 登记进 blocked_tasks，
	 * 切回 prev 后 nesting 仍是被切走前的值(per-thread 跟着线程走).
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
		 * 若本线程在临界区内曾被抢占，rcu_note_context_switch 已把它
		 * 登记进 blocked_tasks.现在退出临界区，需要摘除登记，
		 * 否则 synchronize_rcu 阶段二会永远等不到列表为空。
		 *
		 * 关中断 + 持锁的复合临界区，与 rcu_note_context_switch 的
		 * 锁路径锁序一致(都是先关中断，再拿锁)，避免与本 CPU 的
		 * timer tick / IPI 内潜在的同锁路径冲突。
		 *
		 * 这里手动展开 save_and_disable_interrupts + spin_lock 而不用
		 * spin_lock_irqsave 宏，是因为后者在 inline 展开后会触发
		 * gcc 的 maybe-uninitialized 误报(lock.h 的 inline asm 用
		 * "=r"(flags) 的写法在跨 inline 边界时类型推断不严密)。
		 * 手动展开等价，更直白，也回避了这个误报。
		 */
		if (t->rcu_blocked) {
			uint64_t flags = save_and_disable_interrupts();
			spin_lock(&rcu_state.blocked_lock);
#if CONFIG_RCU_DEBUG
			/* 摘除前快照 */
			L("[RCU UNL] CPU %d t=%s blknode=(%p,%p) head=(%p,%p)",
					ctx->id, t->name,
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

#else  /* !CONFIG_RCU */

/** RCU 关闭时的 no-op stub
 *
 * 目的：保留 reader/updater/集成 三类 API 的符号与签名，让所有调用点
 *        无需 #if 包裹即可继续编译；运行时全部退化为零开销 no-op。
 *
 * rcu.c 在 CONFIG_RCU=0 时只导出空函数，因此 rcu_state 全局符号不存在；
 * 这里也不暴露 rcu_global_state_t / rcu_state 声明。
 */
struct thread;  /* 前向声明：关闭时不引入 thread.h 依赖也能编译 */
static inline void rcu_read_lock(void)   { /* no-op */ }
static inline void rcu_read_unlock(void) { /* no-op */ }
static inline void synchronize_rcu(void) { /* no-op */ }
static inline void rcu_check_quiescent_state(void) { /* no-op */ }
static inline void rcu_note_context_switch(struct thread *prev) { (void)prev; }

#endif /* CONFIG_RCU */

#endif
