/*
 * 内核线程调度器。
 *
 * 调度器按优先级选择就绪线程，并在时钟抢占、主动让出 CPU、阻塞等待等
 * 场景切换执行上下文。每个 CPU 独立维护当前线程和就绪队列。
 */

#include "sched.h"
#include "cpu.h"
#include "gdt.h"
#include "timer.h"
#include "thread.h"
#include "debug.h"
#include "rcu.h"
#include "hhdm.h"
#include "arch_tlb.h"

/* 判断当前 CPU 是否存在可立即兑现的抢占请求。IRQ 返回、系统调用退出和
 * idle 循环均使用此检查；实际是否循环重检由各调用路径决定。 */
int check_need_schedule(void)
{
	struct cpu_context *ctx = cpu_get_ctx();
	/* 启动或平台特殊路径可能尚未建立完整调度上下文；此时不得进入调度器。 */
	if (!ctx || !ctx->current || !ctx->idle) {
		return 0;
	}
	if (atomic_read(&ctx->preempt_count)) {
		return 0;
	}

	if (__atomic_load_n(&ctx->need_resched, __ATOMIC_ACQUIRE)) {
		return 1;
	}

	/* 本函数只报告状态。IRQ 汇编返回路径和 syscall 退出路径会在调度后重检；
	 * idle 循环则在下一轮重新检查。 */

	return 0;
}

void __switch_mm(struct thread *prev, struct thread *next)
{
	struct cpu_context *ctx = cpu_get_ctx();

	if (next->pml4_phys != 0) {
		/* 用户线程使用自己的页表根。 */
		if (ctx->active_pml4_phys != next->pml4_phys) {
			/* 仅在页表根变化时重载 CR3。 */
			arch_write_cr3((uint64_t)next->pml4_phys);
			ctx->active_pml4_phys = next->pml4_phys;
		}
	} else {
		/* 内核线程沿用当前页表根；所有进程共享相同的内核映射。 */
		if (thread_get_status(prev) == THREAD_ZOMBIE) {
			/* 前一线程退出时切回内核根，避免继续依赖已销毁的用户页表。 */
			uint64_t k_pml4_phys = arch_kernel_root_phys();
			if (ctx->active_pml4_phys != (void *)k_pml4_phys) {
				arch_write_cr3(k_pml4_phys);
				ctx->active_pml4_phys = (void *)k_pml4_phys;
			}
		}
	}
}

void __switch_mm_classic(void)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *next = ctx->current;

	/* 兼容实现：用户线程切到私有页表，内核线程使用内核页表。 */
	if (next->pml4_phys != 0) {
		L("CPU %d to user next %p %s %ld pml4 %p", ctx->id, next, next->name, next->id,
			next->pml4_phys);
		uint64_t current_cr3 = arch_read_cr3();
		if (current_cr3 != (uint64_t)next->pml4_phys) {
			/* 仅在 CR3 不同时切换到用户页表。 */
			arch_write_cr3((uint64_t)next->pml4_phys);
		}
	} else {
		/* 该实现为内核线程显式加载内核页表根。 */
		extern uint64_t *kernel_pml4;
		uint64_t k_pml4_phys = virt_to_phys(kernel_pml4);
		L("CPU %d to kernel next %p %s %ld pml4 %p", ctx->id, next, next->name, next->id,
			next->pml4_phys);
		arch_write_cr3(k_pml4_phys);
	}
}

static void switch_mm(struct thread *prev, struct thread *next)
{
#if SWITCH_MM
	__switch_mm(prev, next);
#else
	__switch_mm_classic();
#endif
}

static struct thread *pick_next(struct cpu_context *ctx)
{
	struct thread *prev = ctx->current;
	struct thread *next = NULL;

	int n = atomic64_read(&ctx->runqueue.count);
	if (0 == n) {
		return ctx->idle;
	}

	spin_lock_raw(&ctx->runqueue.lock);

	/* 按优先级位图从最高优先级开始查找可运行线程。 */
	uint64_t bm = ctx->runqueue.bitmap[0];
	int found_prio = -1;
	while (bm) {
		int prio = __builtin_ctzll(bm);
		struct thread *pos = NULL;
		list_for_each_entry(pos, &ctx->runqueue.heads[prio], node) {
			if (THREAD_READY == thread_get_status(pos)) {
				next = pos;
				found_prio = prio;
				break;
			}
			if (unlikely(THREAD_SLEEPING == thread_get_status(pos) &&
				     pos->wakeup_ticks <= (uint64_t)atomic64_read(&g_ticks))) {
				next = pos;
				found_prio = prio;
				break;
			}
		}
		if (next) {
			break;
		}
		bm &= ~(1ULL << prio);
	}

	/* READY 扫描不会选中当前 RUNNING 线程。若没有就绪候选，或候选线程的
	 * 优先级都低于当前线程，则继续运行当前线程。同优先级候选优先执行，
	 * 由此实现同优先级轮转，避免无意义的 prev == next 切换。 */
	if (prev != ctx->idle && THREAD_RUNNING == thread_get_status(prev) &&
		(!next || prev->priority < found_prio)) {
		next = prev;
		found_prio = -1;
	}

	if (likely(next)) {
		if (next != prev) {
			/* 选中的同优先级线程移到队尾，供后续轮转。 */
			list_del(&next->node);
			if (list_empty(&ctx->runqueue.heads[found_prio])) {
				ctx->runqueue.bitmap[0] &= ~(1ULL << found_prio);
			}
			list_add_tail(&next->node, &ctx->runqueue.heads[found_prio]);
			ctx->runqueue.bitmap[0] |= (1ULL << found_prio);
		}
	} else {
		/* 没有可运行候选时选择 idle。队列仍非空表示其中线程都暂不可运行，
		 * 调试构建会输出队列状态以便定位状态或唤醒路径问题。 */
#if CONFIG_RCU_DEBUG
		{
			struct thread *dpos = NULL;
			int idx = 0;
			for (int p = 0; p < SCHED_PRIO_COUNT && idx < 10; p++) {
				list_for_each_entry(dpos, &ctx->runqueue.heads[p], node) {
					L("  rq[%d] prio=%d %s(%ld) status=%d nest=%d blkd=%d", idx,
						p, dpos->name, dpos->id, thread_get_status(dpos),
						dpos->rcu_nesting, dpos->rcu_blocked);
					idx++;
					if (idx >= 10) {
						break;
					}
				}
			}
		}
#endif
		next = ctx->idle;
	}
	/* next 先设 RUNNING 再处理 prev；阻塞类状态保持不变。 */
	thread_set_status(next, THREAD_RUNNING);
	if (prev != next && THREAD_RUNNING == thread_get_status(prev)) {
		thread_set_status(prev, THREAD_READY);
	}
	prev->ticks++;

	/* 跟踪 pick_next 出口时 prev / next 的状态归属。
	 * 过滤范围：除 idle / reader (rcu-rd-*) / updater (rcu-upd) 外全部打印。
	 * 用于判断异常 RUNNING 状态是否源于调度器遗漏状态更新。 */
#if CONFIG_RCU_DEBUG
	{
		const char *pn = prev->name;
		int skip = 0;
		if (prev == ctx->idle) {
			skip = 1;
		} else if (pn[0] == 'r' && pn[1] == 'c' && pn[2] == 'u' && pn[3] == '-') {
			skip = 1;
		}
		if (!skip) {
			L("[PICK OUT] CPU %d prev=%s pst=%d next=%s nst=%d same=%d", ctx->id,
				prev->name, thread_get_status(prev), next->name,
				thread_get_status(next), (prev == next) ? 1 : 0);
		}
	}
#endif

	spin_unlock_raw(&ctx->runqueue.lock);

	return next;
}

static struct thread *sched_idle_zombie(struct cpu_context *ctx)
{
	struct thread *prev = ctx->current;
	struct thread *next = ctx->idle;

	if (THREAD_ZOMBIE == thread_get_status(prev)) {
		spin_lock_raw(&ctx->runqueue.lock);
		list_del(&prev->node);
		/* 优先级队列为空时清除对应位图项。 */
		if (list_empty(&ctx->runqueue.heads[prev->priority])) {
			ctx->runqueue.bitmap[0] &= ~(1ULL << prev->priority);
		}
		atomic64_dec(&ctx->runqueue.count);
		spin_unlock_raw(&ctx->runqueue.lock);

		spin_lock_raw(&ctx->zombiequeue.lock);
		list_add(&prev->node, &ctx->zombiequeue.heads[prev->priority]);
		ctx->zombiequeue.bitmap[0] |= (1ULL << prev->priority);
		atomic64_inc(&ctx->zombiequeue.count);
		spin_unlock_raw(&ctx->zombiequeue.lock);
	}

	if (atomic64_read(&ctx->zombiequeue.count) <= ZOMBIE_MAX) {
		return NULL;
	}

	/* idle 线程在旧线程已离开其栈后回收 zombie；因此有待回收线程时，
	 * 本轮先切到 idle，而不是直接运行普通就绪线程。 */
	L("CPU %d will switch to idle for zombie %p %s %ld", ctx->id, prev, prev->name, prev->id);

	return next;
}

__attribute__((noinline)) static void __schedule(bool preemptive)
{
	/* 调度临界区内关闭本地中断，并保存调用方的原始 IF 状态。 */
	uint64_t flags = save_and_disable_interrupts();

	struct cpu_context *ctx = cpu_get_ctx();

	/*
	 * 调试记录普通线程进入 __schedule 的状态，用于区分未进入调度器与
	 * 调度状态写回错误。idle 和 RCU 专用线程不纳入该诊断输出。
	 */
#if CONFIG_RCU_DEBUG
	{
		struct thread *p = ctx->current;
		const char *n = p->name;
		int skip = 0;
		if (p == ctx->idle) {
			skip = 1;
		} else if (n[0] == 'r' && n[1] == 'c' && n[2] == 'u' && n[3] == '-') {
			skip = 1;
		}
		if (!skip) {
			L("[SCHED IN] CPU %d prev=%s status=%d preempt_cnt=%d preemptive=%d",
				ctx->id, n, thread_get_status(p), atomic_read(&ctx->preempt_count),
				preemptive);
		}
	}
#endif
	if (atomic_read(&ctx->preempt_count)) {
		if (preemptive) {
			L("Scheduling while atomic! count %d return",
				atomic_read(&ctx->preempt_count));
			/* 抢占式请求可延期；恢复调用方中断状态后返回。 */
			restore_interrupts(flags);
			return;
		} else {
			/* 主动阻塞或让出 CPU 时仍处于不可抢占区属于调用错误。 */
			panic("Scheduling while atomic! count %d",
				atomic_read(&ctx->preempt_count));
		}
	}
	__atomic_store_n(&ctx->need_resched, 0, __ATOMIC_RELAXED);

	struct thread *prev = ctx->current;
	struct thread *next = sched_idle_zombie(ctx);
	if (!next) {
		next = pick_next(ctx);
	}
	if (next == prev) {
		restore_interrupts(flags);
		return;
	}

	/*
	 * RCU 集成钩子：在切走 prev 之前登记。
	 *
	 * 调用前提：本 CPU 中断已关，符合 rcu_note_context_switch 的契约，
	 * 其内部用裸 spin_lock 而非 spin_lock_irqsave，依赖调用方保证中断已关。
	 *
	 * 时序选点：
	 *   - next 已选定，确认本次确实会切走 prev；
	 *   - ctx->current 尚未改为 next，prev 仍是当前线程；
	 *   - 地址空间和寄存器上下文尚未切换，仍可对 prev 完成记账。
	 *
	 * 若 prev 仍在 RCU 读侧临界区，将其登记到 blocked_tasks；否则将本次
	 * 切换记为本 CPU 的安静点，并更新 rcu_gp_seq_seen。
	 */
	rcu_note_context_switch(prev);

	/* 在 next 首次从用户态进入内核前，先把该 CPU 的 TSS.RSP0 指向 next 栈顶。 */
	tss_set_rsp0(ctx->id, (uint64_t)next->kernel_stack);

	switch_mm(prev, next);

	/* 与 TTY9 采样共用 runqueue.lock，使采样方读取 current、run_tsc 和
	 * last_tsc 时不会跨越本次更新。当前 CPU 的中断已关闭；裸锁仍用于
	 * 与远端采样方互斥。 */
	uint64_t now = rdtsc();
	spin_lock_raw(&ctx->runqueue.lock);
	prev->run_tsc += now - prev->last_tsc;
	ctx->current = next;
	next->last_tsc = now;
	spin_unlock_raw(&ctx->runqueue.lock);

	/*
	 * FPU/SSE 状态由 switch_to(switch.asm)保存和恢复，与 GPR
	 * 一起在汇编中完成。详见 switch.asm 的 fxsave64/fxrstor64 注释。
	 */
	switch_to(prev, next);

	__asm__ volatile("" ::: "memory");

	restore_interrupts(flags);
}

/*
 * __schedule_irq() - IRQ 即将返回时执行的调度。
 *
 * 由 idt_stubs.S 的 ret_from_intr 路径调用。通用入口完成异常或中断分发后，
 * 若 need_resched 仍置位，汇编入口调用本函数；切换返回后会再次检查该标志。
 */
inline void __schedule_irq(void)
{
	__schedule(true);
}

/*
 * __schedule_preempt() - 兑现 preempt_enable() 推迟下来的抢占请求。
 *
 * preempt_enable() 在 preempt_count 降为零且 need_resched 置位时调用本函数。
 * 若检查标志后、进入本函数前收到定时器中断，IRQ 返回路径会再次检查该标志，
 * 因而请求仍可被兑现。
 */
void __schedule_preempt(void)
{
	cpu_get_ctx()->preempts++;
	get_current()->preempts++;
	__schedule(true);
}

inline __attribute__((always_inline)) int schedule_timeout(uint64_t t)
{
	struct thread *current = cpu_get_ctx()->current;
	if (unlikely(!t || THREAD_RUNNING != thread_get_status(current))) {
		return -1;
	}

	thread_set_status(current, THREAD_SLEEPING);
	/* g_ticks 由 BSP 推进。此处的原子快照允许边界相差一个 tick，只影响
	 * 实际等待时长，不会丢失到期唤醒。 */
	current->wakeup_ticks = atomic64_read(&g_ticks) + t;
	__schedule(false);

	/* 返回当前 tick 超过 wakeup_ticks 的值，即超过目标期限的唤醒延迟。 */
	return atomic64_read(&g_ticks) - current->wakeup_ticks;
}
EXPORT_SYMBOL(schedule_timeout);

inline __attribute__((always_inline)) void schedule(void)
{
	__schedule(false);
}
EXPORT_SYMBOL(schedule);

static void sched_snap_cpu(int id)
{
	struct cpu_context *ctx = g_cpu_contexts[id];
	if (!ctx) {
		panic("NULL ctx");
	}
	/* 以下是逐 CPU 采集的诊断信息，不构成跨 CPU 同一时刻的一致快照。 */
	struct thread *t = ctx->current;
	kprintf(" CPU %d ctx %p current %p %s %d running %ld threads idle(%p) ticks %lu\n", ctx->id,
		ctx, t, t->name, t->id, atomic64_read(&ctx->runqueue.count), ctx->idle,
		ctx->idle->ticks);

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&ctx->runqueue.lock, flags);

	struct thread *pos = NULL;
	int n = 0;
	for (int p = 0; p < SCHED_PRIO_COUNT; p++) {
		list_for_each_entry(pos, &ctx->runqueue.heads[p], node) {
			L("  #%d prio=%d %s(%ld) status %d ticks %lu cycles %lu sleeps %lu "
			  "pml4_phys %p stack %p %p %s",
				n, p, pos->name, pos->id, thread_get_status(pos), pos->ticks,
				pos->run_tsc, pos->sleep_times, pos->pml4_phys, pos->kernel_stack,
				pos->user_stack, THREAD_STATUS_STR[pos->status]);
			n++;
		}
	}

	arch_spin_unlock_irqrestore(&ctx->runqueue.lock, flags);
}

void sched_snap(void)
{
	/* 逐 CPU 输出当前线程与就绪队列状态。 */
	kprintf("SMP %lu\n", g_cpu_count);
	for (int i = 0; i < (int)g_cpu_count; i++) {
		sched_snap_cpu(i);
	}
}
