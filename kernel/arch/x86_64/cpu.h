#ifndef __CPU_H__
#define __CPU_H__

/*
 * x86_64 每 CPU 上下文与处理器管理接口。
 *
 * cpu_context 保存当前处理器的线程、调度队列和抢占状态。GS:16 指向
 * 当前 cpu_context 的 self 字段，cpu_get_ctx() 据此取得本地上下文。
 */

#include <stdint.h>

#include "lock.h"
#include "list.h"
#include "thread.h"
#include "define.h"
#include "atomic.h"

/* 按内核逻辑 CPU 编号索引的上下文表。 */
extern struct cpu_context *g_cpu_contexts[MAX_CPUS];
extern uint32_t g_cpu_count;
extern volatile uint64_t online;

// IA32_GS_BASE
#define MSR_FS_BASE	   0xC0000100
#define MSR_GS_BASE	   0xC0000101
#define MSR_GS_KERNEL_BASE 0xC0000102
/* 长模式下 FS/GS 基址可用于线程本地或每 CPU 数据访问。 */

#define cpu_get_ctx()                                                                              \
	({                                                                                         \
		struct cpu_context *_ctx;                                                          \
		__asm__ volatile("movq %%gs:16, %0" : "=r"(_ctx));                                 \
		_ctx;                                                                              \
	})

#define IDLE_PID 0

#define ZOMBIE_MAX 0

typedef struct {
	/* 保护该 CPU 的就绪队列。 */
	spinlock_t lock;
	/* 每个优先级对应一个就绪队列。 */
	struct list_node heads[SCHED_PRIO_COUNT];
	/* 第 i 位为 1 表示对应优先级队列非空。 */
	uint64_t bitmap[SCHED_PRIO_BITMAP_SZ];
	/* 队列中的就绪线程数。 */
	atomic64_t count;
	uint64_t switches;
} runqueue_t;

struct cpu_context {
	/* TSS.RSP0 使用的内核栈顶。 */
	uint64_t kernel_stack;
	uint64_t user_rsp; /* 偏移 8，保存用户 RSP。 */
	/* 必须是偏移 16，供 gs:16 寻址 */
	void *self; // 指向自己 (gs:16)
	uint64_t kernel_stack_base;
	int32_t id;		/* 内核逻辑 CPU 编号。 */
	atomic_t preempt_count; /* 抢占禁止嵌套计数。 */
	int need_resched;
	uint32_t preempts;

	uint64_t *active_pml4_phys;

	struct thread *current; /* 当前线程。 */
	struct thread *idle;	/* 本 CPU 的 idle 线程。 */
	runqueue_t runqueue;	/* 本 CPU 的就绪队列。 */
	runqueue_t zombiequeue;

	/* 调度统计计数。 */
	uint64_t total_ticks;
	uint64_t idle_ticks;
	/* 上次统计采样的总 tick 与 idle tick，用于计算区间 CPU 占用率。 */
	uint64_t last_total_ticks;
	uint64_t last_idle_ticks;

	/* TTY9 的每 CPU 采样窗口。last_cpu_tsc 与 last_idle_tsc 必须配对，
	 * 这样 Core 行和各线程行共用同一个 TSC 分母。 */
	uint64_t last_cpu_tsc;
	uint64_t last_idle_tsc;

	/* 最近一次观察到的 RCU 宽限期序号。 */
	int rcu_gp_seq_seen;
	struct list_node *task;
	/* 当前中断嵌套深度，供诊断使用。 */
	uint64_t interrupt_nesting_level;

	struct thread bootcb;

	spinlock_t lock;
};

#define CPU_CONTEXT_DUMP(t)                                                                        \
	do {                                                                                       \
		__typeof__(t) _t = (t);                                                            \
		L("CPU Context %p\n"                                                               \
		  "  kernel statck     %p\n"                                                       \
		  "  user rsp          %p\n"                                                       \
		  "  self              %p\n"                                                       \
		  "  kernel stack base %p\n"                                                       \
		  "  cpu id            %d\n"                                                       \
		  "  preempt count     %d\n"                                                       \
		  "  current           %p\n"                                                       \
		  "  idle              %p\n",                                                      \
			_t, (void *)_t->kernel_stack, (void *)_t->user_rsp, _t->self,              \
			(void *)_t->kernel_stack_base, _t->id, atomic_read(&_t->preempt_count),    \
			_t->current, _t->idle);                                                    \
	} while (0)

/* 所有已发现的 CPU 均已上线时为真。 */
#define CPUS_ONLINE() (online == g_cpu_count)

enum cpu_startup_role {
	CPU_STARTUP_BSP,
	CPU_STARTUP_AP,
};

void per_cpu_init(uint32_t cpu_id, enum cpu_startup_role role);
void cpu_assert_sched_context_ready(void);
uint64_t rdmsr(uint32_t msr);
void wrmsr(uint32_t msr, uint64_t value);
void cpu_enqueue(int cpu_id, struct thread *t);
void cpu_enqueue_tail(int cpu_id, struct thread *t);
struct thread *cpu_dequeue(int cpu_id);
void runqueue_enqueue_raw(runqueue_t *rq, struct thread *t);
struct thread *runqueue_dequeue_raw(runqueue_t *rq);
struct thread *runqueue_dequeue_cond_raw(runqueue_t *rq, const int cond);
void runqueue_enqueue_tail_raw(runqueue_t *rq, struct thread *t);
void cpu_enqueue_zombie(int cpu_id, struct thread *t);
struct thread *cpu_dequeue_zombie(int cpu_id);
struct thread *cpu_dequeue_zombie_tail(int cpu_id);
void cpu_early_init_gs(int id);
/* 启用抢占计数时须先安装有效的本地 GS context；两接口必须严格配对。
 * context 缺失或计数下溢属于内核错误，不作为可恢复情况处理。 */
void preempt_disable();
void preempt_enable();
void cpu_test();
void cpu_early_init_gs_test(int id);
void wait_online(int id);

#endif
