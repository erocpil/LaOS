#ifndef __CPU_H__
#define __CPU_H__

/*
 * cpu.h - per-CPU 上下文，preempt_count,need_resched 类型定义
 *
 * 定义 struct cpu_context(每 CPU 一个，GDT/IDT/TSS/runqueue/current_thread
 * 等状态的聚集地).内核其他子系统通过 cpu_get_ctx() 获取本 CPU 上下文。
 */

#include <stdint.h>

#include "lock.h"
#include "list.h"
#include "thread.h"
#include "define.h"
#include "atomic.h"

// 全局 CPU 上下文查找表
extern struct cpu_context* g_cpu_contexts[MAX_CPUS];
extern uint32_t g_cpu_count;
extern volatile uint64_t online;
// extern volatile int cpu_ctx_stage;

// IA32_GS_BASE
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
#define MSR_GS_KERNEL_BASE 0xC0000102
/*
   在 64 位模式下，段寄存器(CS， DS， ES， SS)的基地址(Base Address)通常被忽略(视为 0)，
   但 FS 和 GS 是例外。它们依然可以拥有一个 64 位的基地址，
   常用于存放线程本地存储(Thread Local Storage， TLS)或内核CPU相关数据(Per-CPU data).
   0xC0000100 对应 FS_BASE.
   0xC0000101 对应 GS_BASE .
   */

/*
   ； 获取当前 CPU 的 ID
   mov %gs:8, %eax    ； 假设 cpu_id 在结构体偏移 8 的位置

   ； 获取当前正在运行的线程 TCB
   mov %gs:16, %rax   ； 假设 current_thread 指针在偏移 16
   */

#define cpu_get_ctx() ({ \
		struct cpu_context *_ctx; \
		__asm__ volatile("movq %%gs:16, %0" : "=r"(_ctx)); \
		_ctx; \
		})

/* TODO
   利用 GCC 的 __seg_gs 扩展(代码最干净)
   如果使用的是较新版本的 GCC，它支持"地址空间"属性，这让代码看起来像原生指针：
// 定义在 GS 段的类型
#define PER_CPU __attribute__((address_space(256))) // 256 对应 GS

void some_function() {
// 假设 ctx 结构体就在 GS 的基地址处
struct cpu_context PER_CPU *p = 0;
int id = p->id； // 编译器会自动生成 mov %gs:offset, %eax
}
*/
/* TODO
 * #define __percpu __attribute__((section(".percpu")))
 struct rq __percpu cpu_rq;
 */

#define IDLE_PID 0

#define ZOMBIE_MAX 0

typedef struct {
	// 保护本核队列的锁
	spinlock_t lock;
	// 每优先级一个链表头 (0=最高，SCHED_PRIO_COUNT-1=最低)
	struct list_node heads[SCHED_PRIO_COUNT];
	// bitmap[i]=1 表示优先级 i 的链表非空
	uint64_t bitmap[SCHED_PRIO_BITMAP_SZ];
	// 队列中就绪线程的数量
	atomic64_t count;
	uint64_t switches;
} runqueue_t;

struct cpu_context {
	// 当前 CPU 的系统栈 (用于从用户态切回内核)
	// 偏移 0 (必须是真正的栈顶地址)
	uint64_t kernel_stack;
	uint64_t user_rsp;        // 偏移 8 (专门用来临时存放用户 RSP)
	/* 必须是偏移 16，供 gs:16 寻址 */
	void *self; // 指向自己 (gs:16)
	uint64_t kernel_stack_base;
	int32_t id;               // 当前 CPU 编号
	atomic_t preempt_count;   // 抢占嵌套计数：用 atomic 防 RMW 中间态被
	                          // 中断读到(lock irqsave 段读 0 错失抢占等)
	int need_resched;
	uint32_t preempts;

	uint64_t *active_pml4_phys;

	struct thread *current;    // 当前正在运行的线程 (TCB)
	struct thread *idle;        // 空闲线程 (gs:24)
									   // 就绪队列 (内嵌在结构体中)
	runqueue_t runqueue;      // 当前 CPU 的私有运行队列
	runqueue_t zombiequeue;

	// metric
	uint64_t total_ticks;
	uint64_t idle_ticks;
	// %CPU 抽样快照：上一次 stats 抽样时的 total_ticks / idle_ticks,
	// 用于在固定窗口内做差分得到该核非 idle 占比。仅 stats 读写，无并发风险。
	uint64_t last_total_ticks;
	uint64_t last_idle_ticks;

	/* TTY9 的每 CPU 采样窗口。last_cpu_tsc 与 last_idle_tsc 必须配对，
	 * 这样 Core 行和各线程行共用同一个 TSC 分母。 */
	uint64_t last_cpu_tsc;
	uint64_t last_idle_tsc;

	// rcu_nesting 已迁至 struct thread(per-thread).
	// 原因：方案 B 允许 RCU 临界区内被抢占。若把 nesting 放在 cpu_context,
	// 切线程时状态归属不明:next 不属于 RCU 临界区，却看到 nesting > 0;
	// reader 切回原 CPU 时本应继续之前的嵌套，状态却被中间线程污染。
	// 必须跟随线程走，详见 rcu.h / docs/rcu-cleanup-plan.md Phase R-6.
	int rcu_gp_seq_seen; // 本CPU最后一次确认到的宽限期序号
				  //
	// FIXME XXX
	struct list_node *task;
	// TODO 内核嵌套，调试死锁或栈溢出
	uint64_t interrupt_nesting_level;

	struct thread bootcb;

	spinlock_t lock;
};

#define CPU_CONTEXT_DUMP(t) \
	do { \
		__typeof__(t) _t = (t); \
		L("CPU Context %p\n" \
				"  kernel statck     %p\n" \
				"  user rsp          %p\n" \
				"  self              %p\n" \
				"  kernel stack base %p\n" \
				"  cpu id            %d\n" \
				"  preempt count     %d\n" \
				"  current           %p\n" \
				"  idle              %p\n", \
				_t, (void*)_t->kernel_stack, (void*)_t->user_rsp, _t->self, \
				(void*)_t->kernel_stack_base, _t->id, atomic_read(&_t->preempt_count), \
				_t->current, _t->idle); \
	} while (0)

// check if all done
#define CPUS_ONLINE() (online == g_cpu_count)

void per_cpu_init(uint32_t cpu_id, int flag);
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
void preempt_disable();
void preempt_enable();
void cpu_test();
void cpu_early_init_gs_test(int id);
void wait_online(int id);

#endif
