/*
 * thread.c - 内核线程创建，销毁与调度上下文
 *
 * 提供 thread_create / thread_destroy / thread_exit 等线程管理接口。
 * switch_to 汇编切换在 arch 层实现。
 */

#include "cpu.h"
#include "vmm.h"
#include "sched.h"
#include "heap.h"
#include "string.h"
#include "thread.h"
#include "define.h"
#include "atomic.h"
#include "debug.h"
#include "module_alloc.h"
#include "vma.h"
#include "user_vmm.h"
#include "ipi.h"

struct thread *g_current_thread;

atomic64_t g_thread_kernel_count = ATOMIC64_INIT(0);
atomic64_t g_thread_user_count = ATOMIC64_INIT(0);

char *THREAD_STATUS_STR[] = {
	" P  . ", " R  * ", " B  * ", " S  & ", " Z  # ", " E  ~ ",
};

static uint64_t thread_pi_lock(struct thread *t)
{
	uint64_t flags = save_and_disable_interrupts();

	while (__sync_lock_test_and_set(&t->pi_lock.counter, 1))
		cpu_relax();

	return flags;
}

static void thread_pi_unlock(struct thread *t, uint64_t flags)
{
	__sync_lock_release(&t->pi_lock.counter);
	restore_interrupts(flags);
}

static int thread_effective_priority(struct thread *t)
{
	int priority = t->base_priority;

	for (int i = 0; i < SCHED_PRIO_COUNT; i++) {
		if (t->pi_donations[i]) {
			priority = i < priority ? i : priority;
			break;
		}
	}

	return priority;
}

/*
 * Change the runqueue bucket together with the effective priority.  A thread
 * remains linked in its CPU runqueue while RUNNING/BLOCKED/SLEEPING, so a
 * priority change must migrate the node instead of merely changing the TCB.
 */
static void thread_apply_effective_priority(struct thread *t, int priority)
{
	int old_priority = t->priority;
	int cpu = t->target_cpu;

	if (old_priority == priority)
		return;
	if (old_priority < 0 || old_priority >= SCHED_PRIO_COUNT)
		panic("invalid old priority %d for %s %ld",
				old_priority, t->name, t->id);

	if (cpu < 0 || (uint32_t)cpu >= g_cpu_count ||
			!g_cpu_contexts[cpu] || !t->node.next || !t->node.prev ||
			list_empty(&t->node)) {
		t->priority = (uint8_t)priority;
		return;
	}

	runqueue_t *rq = &g_cpu_contexts[cpu]->runqueue;
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&rq->lock, flags);

	/* Recheck after taking the queue lock: an exiting thread may have been
	 * detached while this CPU waited for the lock. */
	if (t->node.next && t->node.prev && !list_empty(&t->node)) {
		list_del(&t->node);
		if (list_empty(&rq->heads[old_priority]))
			rq->bitmap[0] &= ~(1ULL << old_priority);
		t->priority = (uint8_t)priority;
		list_add_tail(&t->node, &rq->heads[priority]);
		rq->bitmap[0] |= 1ULL << priority;
	} else {
		t->priority = (uint8_t)priority;
	}

	arch_spin_unlock_irqrestore(&rq->lock, flags);
	ipi_reschedule_cpu((uint32_t)cpu);
}

void thread_priority_init(struct thread *t)
{
	if (!t)
		return;

	t->priority = SCHED_DEFAULT_PRIO;
	t->base_priority = SCHED_DEFAULT_PRIO;
	memset(t->pi_donations, 0, sizeof(t->pi_donations));
	atomic_set(&t->pi_lock, 0);
}

int thread_set_priority(struct thread *t, int priority)
{
	if (!t || priority < 0 || priority >= SCHED_PRIO_COUNT)
		return -1;

	/* A blocked waiter contributes its current effective priority to a
	 * mutex owner.  Changing it in place would make that donation stale. */
	if (thread_get_status(t) == THREAD_BLOCKED)
		return -2;

	uint64_t flags = thread_pi_lock(t);
	t->base_priority = (uint8_t)priority;
	thread_apply_effective_priority(t, thread_effective_priority(t));
	thread_pi_unlock(t, flags);

	return 0;
}
EXPORT_SYMBOL(thread_set_priority);

void thread_priority_update_donation(struct thread *t,
		int old_priority, int new_priority)
{
	if (!t || old_priority < 0 || old_priority > SCHED_PRIO_NONE ||
			new_priority < 0 || new_priority > SCHED_PRIO_NONE)
		panic("invalid priority donation %d -> %d", old_priority,
				new_priority);

	uint64_t flags = thread_pi_lock(t);

	if (old_priority < SCHED_PRIO_COUNT) {
		if (!t->pi_donations[old_priority])
			panic("missing priority donation %d for %s %ld",
					old_priority, t->name, t->id);
		t->pi_donations[old_priority]--;
	}
	if (new_priority < SCHED_PRIO_COUNT) {
		if (t->pi_donations[new_priority] == UINT16_MAX)
			panic("priority donation overflow for %s %ld",
					t->name, t->id);
		t->pi_donations[new_priority]++;
	}

	thread_apply_effective_priority(t, thread_effective_priority(t));
	thread_pi_unlock(t, flags);
}

void thread_destroy(struct thread *t)
{
	if (!t) {
		return;
	}

	// 1. 释放 ELF 文件副本：EXEC 路径用 kmalloc → kfree；
	//    模块路径用 module_alloc（MODULE_VBASE 区 bump 分配器），暂不回收。
	if (t->elf_load_addr) {
		uint64_t addr = (uint64_t)t->elf_load_addr;
		if (!module_region_contains(addr)) {
			kfree(t->elf_load_addr);
		}
	}

	// 2. 销毁用户虚拟地址空间。vmm_destroy_level() 会同时释放所有
	//    叶映射的物理页，包括 user_stack_phys，不能在此提前重复释放。
	//    VMA 链表节点（kmalloc）在 vmm 销毁前清理。
	vma_destroy_all(t);
	if (t->pml4_phys != 0) {
		arch_user_vmm_destroy(t->pml4_phys);
		// 传入 PML4 物理地址，从第 4 层开始递归
		vmm_destroy_level((uint64_t)t->pml4_phys, 4);
	}

	// 3. 释放内核栈 (kmalloc 分配；静态 TCB 跳过)
	if (t->kernel_stack && !t->is_idle) {
		kfree(t->kernel_stack_base);
	}

	// 4. 释放线程控制块 (TCB) 结构体本身
	//    静态分配的 TCB (is_idle, 如 Limine 路径的 user/idle/boot)
	//    不 kfree；仅 kmalloc 分配的 TCB 需要释放。
	if (!t->is_idle) {
		kfree(t);
	}
}

struct thread *thread_create_common(void (*entry)(void*), void *data)
{
	struct thread *t = (struct thread*)kmalloc(sizeof(struct thread));

	if (!t) {
		return NULL;
	}

	memset(t, 0, sizeof(struct thread));
	arch_fpu_reset_state(t->fpu_state);
	INIT_LIST_NODE(&t->node);
	INIT_LIST_NODE(&t->wait_node);
	thread_priority_init(t);

	/* 任意 CPU */
	t->target_cpu = -1;
	t->tty_id = 0;

	/* 内核栈:STACK_SIZE = 128KB(define.h)，与 KERNEL_STACK_SIZE 保持一致，
	 * 为异常入口和应用线程预留足够栈空间。 */
	void *stack = kmalloc(STACK_SIZE + 16);
	if (!stack) {
		panic("kmalloc(kernel_stack_base)");
	}
	t->kernel_stack_base = stack;

	// 计算栈顶 (rsp0)
	// x86_64 栈顶必须 16 字节对齐
	uint64_t stack_top = (uint64_t)stack + STACK_SIZE;
	// 强制 16 字节对齐，防止某些指令(如 SSE)触发异常
	stack_top &= ~0x0FULL;
	t->kernel_stack = (void*)stack_top;
	t->entry_point = (uint64_t)entry;

	t->rsp = arch_thread_init_frame(stack_top, entry, data);

	t->data = data;
	t->self = t;
	thread_set_status(t, THREAD_READY);
	L("Thread %p id %ld: stack=%p, aligned_top=%p, rsp=%p..%p",
			t, t->id, stack, (void*)stack_top, (void*)t->rsp,
			(void*)(t->rsp + (stack_top - t->rsp)));
	// 检查对齐
	if ((t->rsp & 0xF) != 0) {
		panic("WARNING: Thread %ld RSP not aligned!", t->id);
	}
	L("thread %p %s %ld node %p %p wait_node %p %p",
			t, t->name, t->id, t->node.prev, t->node.next,
			t->wait_node.prev, t->wait_node.next);

	return t;
}

/**
 * thread_entry_point() - 线程统一入口 trampoline
 *
 * 双路径：
 *   TCB 路径(entry_argv!=NULL)：task.conf 加载的模块，参数从 TCB 取。
 *   旧线程路径：thread_create() 直接创建的线程；x86_64 从 R15 取入口，
 *   ARM64 由 ret_from_fork 显式传入 entry_func。
 */
void thread_entry_point(void *data, void *entry_func)
{
	struct thread *t = get_current();

	if (!t->is_user && t->entry_argv) {
		/* TCB 路径：调用 main(argc, argv) */
		int (*main_func)(int, char**) = (void*)t->entry_point;

		/* A configured module is a normal schedulable kernel thread on
		 * every CPU.  Keeping AP IRQs masked here would let one module
		 * monopolize an AP and make remote runqueue wakeups ineffective. */
		arch_local_irq_enable();
		int code = main_func(t->entry_argc, t->entry_argv);
		thread_exit(code);
		__builtin_unreachable();
	}

	/* 旧路径：thread_create() 直接创建的线程。
	 * 入口函数由 arch_thread_get_entry 从架构约定位置获取：
	 *   - ARM64: ret_from_fork 通过 x1 显式传入
	 *   - x86_64: switch.S 通过 R15 走私 */
	int (*func)(void*);
	func = (int (*)(void*))arch_thread_get_entry(entry_func);

	arch_local_irq_enable();

	int code = func(data);

	thread_exit(code);

	__builtin_unreachable();
}

struct thread *thread_create(void (*entry)(void*), void *data)
{
	struct thread *t = NULL;

	t = thread_create_common(entry, data);
	if (t) {
		t->id = THREAD_SET_KERNEL_PID();
	}

	return t;
}

struct thread *thread_create_on(void (*entry)(void*), void *data, int cpu)
{
	struct thread *t = NULL;

	t = thread_create(entry, data);
	if (t) {
		t->target_cpu = cpu;
	}

	return t;
}
EXPORT_SYMBOL(thread_create_on);

void thread_exit(int exit_code)
{
	struct thread *t = get_current();
	struct cpu_context *ctx = cpu_get_ctx();
	L("Thread %s %ld (CPU %d ticks %ld sleeps %ld) exiting with code %d (%p %p)",
			t->name, t->id, cpu_get_ctx()->id, t->ticks, t->sleep_times,
			exit_code, t->node.prev, t->node.next);

	// 关闭本地中断并保存原中断状态
	uint64_t flags = save_and_disable_interrupts();

	if (atomic_read(&ctx->preempt_count)) {
		panic("Thread %p %s %ld exit with preempt_count %d",
				t, t->name, t->id, atomic_read(&ctx->preempt_count));
	}

	// 标记为僵尸状态，不再参与调度
	thread_set_status(t, THREAD_ZOMBIE);

	/*
	 * 计数与入队由 sched_idle_zombie 原子绑定完成：
	 *   1. 这里仅置 THREAD_ZOMBIE 状态，标记"线程已死".
	 *   2. 真正的 zombiequeue 入队和 atomic64_inc 在 sched_idle_zombie 内
	 *      与 list_add 紧邻执行，保证 count 永远等于队列实际长度。
	 *   3. idle_task_function 依据 ctx->zombiequeue.count 决定要清理多少
	 *      节点；count 与队列长度一致是 idle 不取出 NULL 的前提。
	 */

	// 物理页可以释放，但内核栈必须由"下一个"线程来释放
	t->exit_code = exit_code;

	restore_interrupts(flags);

	// 触发调度，把 CPU 交给别人
	schedule();

	// 永远不会执行到这里
	__builtin_unreachable();
}
EXPORT_SYMBOL(thread_exit);

/**
 * thread_get_current_id() - 通过 per-CPU 偏移获取线程 ID。
 *
 * x86_64 通过 GS 段寻址 cpu_context，offsetof(current)==16。
 * 见 arch/x86_64/thread_arch.h : arch_get_percpu_offset()。
 */
inline uint64_t thread_get_current_id()
{
	return arch_get_percpu_offset(16);
}

int thread_set_name(struct thread *t, const char *name)
{
	if (strlen(name) > THREAD_NAME_MAX) {
		return -1;
	}

	memcpy(t->name, name, strlen(name) + 1);

	return 0;
}
EXPORT_SYMBOL(thread_set_name);

inline void thread_set_target_cpu(struct thread *t, int32_t cpu_id)
{
	t->target_cpu = cpu_id;
}

/**
 * thread_setup_user_frame() — 为裸 TCB 填入 ARM64 用户线程栈帧
 *
 * 用于 main.c / limine_main.c 的嵌入式 ELF 用户线程创建路径。
 * entry_func 是内核态入口跳板（如 elf_user_entry），
 * user_arg 作为参数传入（通常是 ELF 用户态入口地址）。
 *
 * 栈帧布局匹配 thread_create_common 的 ARM64 12-slot callee-saved 格式，
 * x19=entry_func, x20=user_arg, x30=ret_from_fork。 */
void thread_setup_user_frame(struct thread *t, void *stack_base,
		void (*entry_func)(void*), uint64_t user_arg)
{
	uint64_t stack_top = (uint64_t)stack_base + KERNEL_STACK_SIZE;
	stack_top &= ~0xFULL;
	t->kernel_stack = (void *)stack_top;
	t->kernel_stack_base = stack_base;

	uint64_t *frame = (uint64_t*)(stack_top - 12 * 8); /* 12 slots */

	frame[0]  = 0;                       /* x29 (FP termination) */
	frame[1]  = (uint64_t)ret_from_fork; /* x30 (LR) */
	frame[2]  = 0;                       /* x27 */
	frame[3]  = 0;                       /* x28 */
	frame[4]  = 0;                       /* x25 */
	frame[5]  = 0;                       /* x26 */
	frame[6]  = 0;                       /* x23 */
	frame[7]  = 0;                       /* x24 */
	frame[8]  = 0;                       /* x21 */
	frame[9]  = 0;                       /* x22 */
	frame[10] = (uint64_t)entry_func;    /* x19 = entry */
	frame[11] = user_arg;                /* x20 = data */

	t->rsp = (uint64_t)frame;
}
