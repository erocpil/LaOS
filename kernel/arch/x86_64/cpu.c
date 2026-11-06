/*
 * cpu.c - per-CPU 上下文管理与 SMP 启动协调
 *
 * 管理 g_cpu_contexts 查找表，boot CPU 上下文初始化，
 * preempt_disable/enable,SMP wait_online 协议。
 */

#include "cpu.h"
#include "gdt.h"
#include "heap.h"
#include "lock.h"
#include "sched.h"
#include "thread.h"
#include "string.h"
#include "debug.h"
#include "log.h"
#include "arch_barrier.h"
#include "export.h"
#include "ipi.h"

// 全局 CPU 上下文查找表
struct cpu_context *g_cpu_contexts[MAX_CPUS] = {NULL};
EXPORT_SYMBOL(g_cpu_contexts);
uint32_t g_cpu_count = -1;
volatile uint64_t online = 0;
EXPORT_SYMBOL(online);

/* 静态槽位不依赖堆分配，每个 CPU 在进入正式上下文前独占一个槽位。 */
struct early_cpu_slot {
	struct cpu_context context;
} __attribute__((aligned(64)));

static struct early_cpu_slot early_cpu_contexts[MAX_CPUS];

void wait_online(int id)
{
	/*
	 * online++ 在多核并发下不是原子(编译为 add [mem]，1 无 lock 前缀，
	 * 或 mov/inc/mov 三条指令)，会丢更新导致 online 终值 < g_cpu_count，
	 * 所有核永远自旋。改用 GCC builtin 的 SEQ_CST RMW 保证原子加。
	 */
	__atomic_fetch_add(&online, 1, __ATOMIC_SEQ_CST);

	L_TAG(LOG_CPU, "CPU %d is standing by.\n", id);

	unsigned long spins = 0;
	while (__atomic_load_n(&online, __ATOMIC_SEQ_CST) != g_cpu_count) {
		if ((spins++ & ((1UL << 20) - 1)) == 0) {
			L("CPU #%d waiting (online=%lu/%lu)", id, online, g_cpu_count);
		}
		__asm__ volatile("pause");
	}

	if (!id) {
		L_TAG(LOG_CPU, "all CPUs are online.\n");
	}
}

void cpu_early_init_gs_test(int cpu_id)
{
	struct cpu_context *ctx1 = cpu_get_ctx();
	struct cpu_context *ctx2 = (struct cpu_context *)rdmsr(MSR_GS_BASE);
	if (ctx1 != ctx2 || !ctx1 || ctx1->self != ctx1 || ctx1->id != cpu_id) {
		panic("cpu %d context not correctly set.", cpu_id);
	}
}

/*
 * cpu_early_init_gs() - 设置 GS_BASE，使 cpu_get_ctx() 可用。
 *
 * 每个逻辑 CPU 使用独立静态槽位。首次调用在安装 GS 前初始化 self 和 id；
 * 同一 CPU 的重复早期调用保留已经建立的状态。per_cpu_init() 随后分配并安装
 * 正式上下文，此后不应再次调用本接口。本接口在本地 IRQ 关闭时调用。
 */
void cpu_early_init_gs(int cpu_id)
{
	if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
		panic("invalid early CPU id %d", cpu_id);
	}
	struct cpu_context *ctx = &early_cpu_contexts[cpu_id].context;
	if (!ctx->self) {
		memset(ctx, 0, sizeof(*ctx));
		ctx->self = ctx;
		ctx->id = cpu_id;
	}
	wrmsr(MSR_GS_BASE, (uint64_t)ctx);
	wrmsr(MSR_GS_KERNEL_BASE, 0);
	cpu_early_init_gs_test(cpu_id);
}

void arch_cpu_early_init(void)
{
	cpu_early_init_gs(cpu_get_ctx()->id);
}

inline void preempt_disable(void)
{
#if PREEMPT
	struct cpu_context *ctx = cpu_get_ctx();
	if (!ctx) {
		panic("preempt_disable requires a local CPU context");
	}
	smp_mb();
	// L("CPU %d ctx %p pid %d count %d need_resched %d", ctx->id, ctx, ctx->id,
	// atomic_read(&ctx->preempt_count) + 1, ctx->need_resched);
	atomic_inc(&ctx->preempt_count);
	smp_mb();
#endif
}

inline void preempt_enable(void)
{
#if PREEMPT
	struct cpu_context *ctx = cpu_get_ctx();
	if (!ctx) {
		panic("preempt_enable requires a local CPU context");
	}
	smp_mb();
	// L("cpu %d ctx %p pid %d count %d need_resched %d", ctx->id, ctx, ctx->id,
	// atomic_read(&ctx->preempt_count) - 1, ctx->need_resched);
	if (atomic_read(&ctx->preempt_count) == 0) {
		// 如果已经是 0 还要减，说明逻辑彻底乱了
		panic("preempt_count underflow! Potential double unlock.");
	}
	atomic_dec(&ctx->preempt_count);
	smp_mb();

	/*
	 * deferred preemption:抢占请求在 lock 内被推迟到这里兑现。
	 * 双重条件：
	 *   1. preempt_count 降到 0  : 不在嵌套临界区里切走
	 *   2. need_resched 真的被设置 : 没人请求抢占就不强切
	 * 漏第 2 条会让每次 spin_unlock 都触发 __schedule，紧密 lock
	 * 链路(如 e1000 RX 流水线)性能会被打爆。
	 */
	if (unlikely(atomic_read(&ctx->preempt_count) == 0 && ctx->need_resched)) {
		__schedule_preempt();
	}
#endif
}

void cpu_test(void)
{
	struct cpu_context *ctx = cpu_get_ctx();
	CPU_CONTEXT_DUMP(ctx);
}

void runqueue_init(runqueue_t *rq)
{
	for (int i = 0; i < SCHED_PRIO_COUNT; i++) {
		list_init(&rq->heads[i]);
	}
	memset(rq->bitmap, 0, sizeof(rq->bitmap));
	spin_lock_init(&rq->lock);
	atomic64_set(&rq->count, 0);
}

/*
 * rdmsr() - 读取 64 位模型特定寄存器 (MSR)
 *
 * msr: 索引地址(例如 0xC0000082 代表 LSTAR)。
 * 返回值: 64 位值，由低 32 位和高 32 位拼接而成。
 */
inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t low, high;
	// 'c' 约束将 msr 放入 rcx
	// 'a' 约束对应 eax， 'd' 约束对应 edx
	asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32) | low;
}

/* wrmsr() - 写入 64 位模型特定寄存器 (MSR)
 *
 * msr: 索引地址。
 * value: 待写入的 64 位值。低 32 位和高 32 位分别写入 eax/edx.
 */
inline void wrmsr(uint32_t msr, uint64_t value)
{
	uint32_t low = (uint32_t)value;
	uint32_t high = (uint32_t)(value >> 32);
	asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr) : "memory");
}

static void per_cpu_init_idle(struct cpu_context *ctx)
{
	struct thread *t = thread_create_idle(idle_task_function, NULL);
	if (!t) {
		panic("CPU %u cannot create idle thread", ctx->id);
	}

	/* thread_create_common() 已分配并初始化 idle 线程栈。 */
	t->target_cpu = ctx->id;
	thread_set_status(t, THREAD_READY);
	memcpy(t->name, "idle", 5);
	ctx->idle = t;

	L("CPU %d Initialized idle", ctx->id);
}

void cpu_assert_sched_context_ready(void)
{
	struct cpu_context *ctx = cpu_get_ctx();
	if (!ctx || ctx->self != ctx || ctx->id < 0 || ctx->id >= MAX_CPUS ||
		g_cpu_contexts[ctx->id] != ctx || !ctx->current || !ctx->idle ||
		ctx->current == ctx->idle || ctx->current->self != ctx->current ||
		ctx->idle->self != ctx->idle || ctx->current->target_cpu != ctx->id ||
		ctx->idle->target_cpu != ctx->id ||
		thread_get_status(ctx->current) != THREAD_RUNNING ||
		thread_get_status(ctx->idle) != THREAD_READY || !ctx->kernel_stack ||
		!ctx->idle->kernel_stack || atomic_read(&ctx->preempt_count)) {
		panic("CPU scheduler context is incomplete before IRQ enable");
	}
}

void per_cpu_init(uint32_t cpu_id, enum cpu_startup_role role)
{
	if (cpu_id >= MAX_CPUS) {
		panic("invalid CPU id %u", cpu_id);
	}
	struct cpu_context *early = cpu_get_ctx();
	if (!early || atomic_read(&early->preempt_count)) {
		panic("CPU %u cannot replace an invalid or active early context", cpu_id);
	}
	L("CPU %u", cpu_id);
	struct cpu_context *ctx = kmalloc(sizeof(struct cpu_context));

	if (!ctx) {
		panic("vmalloc(ctx)");
	}

	memset(ctx, 0, sizeof(struct cpu_context));

	// 为每个 CPU 分配一个独立的安全内核栈 (用于系统调用入口)
	void *kernel_stack_base = kmalloc(KERNEL_STACK_SIZE);
	if (!kernel_stack_base) {
		panic("CPU %u cannot allocate its syscall stack", cpu_id);
	}
	ctx->kernel_stack_base = (uint64_t)kernel_stack_base;
	uint64_t kernel_stack = (uint64_t)kernel_stack_base + KERNEL_STACK_SIZE;
	kernel_stack &= ~0xFULL;
	ctx->kernel_stack = kernel_stack;

	/* 将本 CPU 的内核栈顶写入 TSS，确保 Ring 0 入口使用有效的 RSP0。 */
	tss_set_rsp0(cpu_id, kernel_stack);

	L("CPU %d ctx %p kernel_stack %p kernel_stack_base %p\n", cpu_id, ctx,
		(void *)ctx->kernel_stack, (void *)ctx->kernel_stack_base);

	/*
	 * GS 段基址配对流(swapgs 不变式)
	 *
	 * x86_64 有两个 GS 相关 MSR:
	 *   MSR_GS_BASE        (0xC0000101) : 当前 %gs 的基址
	 *   MSR_GS_KERNEL_BASE (0xC0000102) : swapgs 的交换目标
	 *
	 * swapgs 将两者互换。内核利用这一对维护不变量：
	 *
	 *   内核态：  GS_BASE = ctx,  KERNEL_GS_BASE = 0
	 *   用户态：  GS_BASE = 0,    KERNEL_GS_BASE = ctx
	 *
	 * 即：偶数次 swapgs 后内核有 %gs 访问权，奇数次后用户态 GS 为 0。
	 *
	 * 具体流程：
	 *   初始化(此处)              GS_BASE=ctx,  KERN_GS_BASE=0
	 *   进入用户态 (elf_loader)   swapgs ->     GS_BASE=0,    KERN_GS_BASE=ctx
	 *   用户态中断 (idt_stubs)    swapgs ->     GS_BASE=ctx,  KERN_GS_BASE=0   <- 恢复 percpu
	 *   中断返回 (idt_stubs)      swapgs ->     GS_BASE=0,    KERN_GS_BASE=ctx
	 *
	 * KERNEL_GS_BASE 初始为 0 而非 ctx 的原因：
	 * 内核模式启动时还未进入用户态，swapgs 尚未发生。如果此处设
	 * KERNEL_GS_BASE = ctx，第一次 swapgs 进用户态时 GS_BASE=ctx,
	 * KERNEL_GS_BASE=0，第二次 swapgs(中断入口)GS_BASE=0 即
	 * 内核丢失 percpu 访问。设为 0 保证第一次 swapgs 后 KERNEL_GS_BASE
	 * 正确持有 ctx，为后续中断入口的 swapgs 做好准备。
	 *
	 * 注意:switch_to 不触摸这两个 MSR: 同一 CPU 上的内核线程
	 * 共享同一个 per-CPU context,GS_BASE 不变。
	 */
	ctx->self = ctx;
	ctx->id = cpu_id;
	runqueue_init(&ctx->runqueue);
	runqueue_init(&ctx->zombiequeue);

	spin_lock_init(&ctx->lock);
	/* 正式上下文的本地基础字段就绪后才安装 GS。 */
	wrmsr(MSR_GS_BASE, (uint64_t)ctx);
	wrmsr(MSR_GS_KERNEL_BASE, 0);

	// 存入全局表，以便其他核访问
	g_cpu_contexts[cpu_id] = ctx;

	per_cpu_init_idle(ctx);
	L("per-cpu context %p initialized for %d", ctx, cpu_id);

	if (role == CPU_STARTUP_AP) {
		// 创建一个临时的引导 TCB， 让第一次 schedule() 有地方存放当前的寄存器
		struct thread *boot_tcb = &ctx->bootcb;
		memset(boot_tcb, 0, sizeof(struct thread));
		boot_tcb->self = boot_tcb;
		boot_tcb->target_cpu = (int)cpu_id;
		boot_tcb->id = THREAD_SET_KERNEL_PID();
		INIT_LIST_NODE(&boot_tcb->node);
		INIT_LIST_NODE(&boot_tcb->wait_node);
		thread_priority_init(boot_tcb);
		thread_set_status(boot_tcb, THREAD_RUNNING);
		memcpy(boot_tcb->name, "boot", 5);
		boot_tcb->is_idle = true;
		ctx->current = boot_tcb;
		L("ctx %d set boot tcb %p\n", ctx->id, boot_tcb);
	} else if (role != CPU_STARTUP_BSP) {
		panic("CPU %u has invalid startup role %d", cpu_id, role);
	}
}

void runqueue_enqueue_raw(runqueue_t *rq, struct thread *t)
{
	uint8_t prio = t->priority;
	list_add(&t->node, &rq->heads[prio]);
	rq->bitmap[0] |= (1ULL << prio);
	atomic64_inc(&rq->count);
}

// 将线程加入特定 CPU 的就绪队列
static void runqueue_enqueue(runqueue_t *rq, struct thread *t)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&rq->lock, flags);

	runqueue_enqueue_raw(rq, t);

	arch_spin_unlock_irqrestore(&rq->lock, flags);
}

void runqueue_enqueue_tail_raw(runqueue_t *rq, struct thread *t)
{
	uint8_t prio = t->priority;
	list_add_tail(&t->node, &rq->heads[prio]);
	rq->bitmap[0] |= (1ULL << prio);
	atomic64_inc(&rq->count);
}

static void runqueue_enqueue_tail(runqueue_t *rq, struct thread *t)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&rq->lock, flags);

	runqueue_enqueue_tail_raw(rq, t);

	arch_spin_unlock_irqrestore(&rq->lock, flags);
}

struct thread *runqueue_dequeue_raw(runqueue_t *rq)
{
	/* 从优先级最高的非空队列头部取出线程。 */
	int prio = __builtin_ctzll(rq->bitmap[0]);
	struct thread *t = list_first_entry(&rq->heads[prio], struct thread, node);
	list_del(&t->node);
	if (list_empty(&rq->heads[prio])) {
		rq->bitmap[0] &= ~(1ULL << prio);
	}
	atomic64_dec(&rq->count);

	return t;
}

struct thread *runqueue_dequeue_cond_raw(runqueue_t *rq, const int cond)
{
	smp_mb();

	struct thread *t = NULL;

	for (int prio = 0; prio < SCHED_PRIO_COUNT; prio++) {
		struct thread *pos = NULL;
		list_for_each_entry(pos, &rq->heads[prio], node) {
			if (thread_get_status(pos) == (thread_status_t)cond) {
				t = pos;
				break;
			}
		}
		if (t) {
			list_del(&t->node);
			if (list_empty(&rq->heads[prio])) {
				rq->bitmap[0] &= ~(1ULL << prio);
			}
			atomic64_dec(&rq->count);
			return t;
		}
	}

	return NULL;
}

/* 从就绪队列头部取出一个线程。 */
static struct thread *runqueue_dequeue(runqueue_t *rq)
{
	uint64_t flags;
	arch_spin_lock_irqsave(&rq->lock, flags);

	if (!atomic64_read(&rq->count)) {
		arch_spin_unlock_irqrestore(&rq->lock, flags);
		return NULL;
	}

	struct thread *t = runqueue_dequeue_raw(rq);

	arch_spin_unlock_irqrestore(&rq->lock, flags);

	return t;
}

struct thread *runqueue_dequeue_tail_raw(runqueue_t *rq)
{
	/* 从优先级最高的非空队列尾部取出线程。 */
	int prio = __builtin_ctzll(rq->bitmap[0]);
	struct thread *t = list_last_entry(&rq->heads[prio], struct thread, node);
	list_del(&t->node);
	if (list_empty(&rq->heads[prio])) {
		rq->bitmap[0] &= ~(1ULL << prio);
	}
	atomic64_dec(&rq->count);

	return t;
}

static struct thread *runqueue_dequeue_tail(runqueue_t *rq)
{
	uint64_t flags;
	arch_spin_lock_irqsave(&rq->lock, flags);

	if (!atomic64_read(&rq->count)) {
		arch_spin_unlock_irqrestore(&rq->lock, flags);
		return NULL;
	}

	struct thread *t = runqueue_dequeue_tail_raw(rq);

	arch_spin_unlock_irqrestore(&rq->lock, flags);

	return t;
}

static void cpu_queue_check(int cpu_id, struct thread *t)
{
	if (cpu_id != t->target_cpu) {
		panic("no cpu specified %d %d for %s %ld", cpu_id, t->target_cpu, t->name, t->id);
	}
}

void cpu_enqueue(int cpu_id, struct thread *t)
{
	if (-1 == cpu_id) {
		// 使用线程记录的目标 CPU。
		cpu_id = t->target_cpu;
	}
	cpu_queue_check(cpu_id, t);

	// 定位目标 CPU 的运行队列。
	struct cpu_context *ctx = g_cpu_contexts[cpu_id];

	// 发布线程就绪状态后，再将其加入目标运行队列。
	thread_set_status(t, THREAD_READY);

	runqueue_enqueue(&ctx->runqueue, t);
	ipi_reschedule_cpu((uint32_t)cpu_id);
}
EXPORT_SYMBOL(cpu_enqueue);

void cpu_enqueue_tail(int cpu_id, struct thread *t)
{
	cpu_queue_check(cpu_id, t);

	// 快速定位目标核的 rq
	struct cpu_context *ctx = g_cpu_contexts[cpu_id];
	t->target_cpu = cpu_id;

	thread_set_status(t, THREAD_READY);

	runqueue_enqueue_tail(&ctx->runqueue, t);
	ipi_reschedule_cpu((uint32_t)cpu_id);
}
EXPORT_SYMBOL(cpu_enqueue_tail);

struct thread *cpu_dequeue(int cpu_id)
{
	struct cpu_context *ctx = g_cpu_contexts[cpu_id];
	struct thread *t = runqueue_dequeue(&ctx->runqueue);

	return t;
}

void cpu_enqueue_zombie(int cpu_id, struct thread *t)
{
	cpu_queue_check(cpu_id, t);

	struct cpu_context *ctx = g_cpu_contexts[cpu_id];
	t->target_cpu = cpu_id;

	thread_set_status(t, THREAD_READY);

	runqueue_enqueue(&ctx->zombiequeue, t);
}

struct thread *cpu_dequeue_zombie(int cpu_id)
{
	struct cpu_context *ctx = g_cpu_contexts[cpu_id];
	return runqueue_dequeue(&ctx->zombiequeue);
}

struct thread *cpu_dequeue_zombie_tail(int cpu_id)
{
	struct cpu_context *ctx = g_cpu_contexts[cpu_id];
	return runqueue_dequeue_tail(&ctx->zombiequeue);
}
