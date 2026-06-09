/*
 * sched.c - 内核调度器
 *
 * 基于优先级的可抢占调度器。调度时机：时钟 tick，
 * 进程主动 sleep，mutex 等待，yield.
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

/** check_need_schedule() - IRQ 返回路径唯一的抢占决策点 */
int check_need_schedule(void)
{
	struct cpu_context* ctx = cpu_get_ctx();
	/* IRQs are enabled only after scheduler initialization on ARM64, but
	 * retain this guard so an early/platform interrupt cannot dereference
	 * an incomplete boot context. */
	if (!ctx || !ctx->current || !ctx->idle) {
		return 0;
	}
	if (atomic_read(&ctx->preempt_count)) {
		return 0;
	}

	if (__atomic_load_n(&ctx->need_resched, __ATOMIC_ACQUIRE)) {
		return 1;
	}

	// 只要有抢占标记，就一直调度，直到没有标记为止。
	// 在 idt_stubs.S 的 ret_from_intr 中实现。

	return 0;
}

void __switch_mm(struct thread *prev, struct thread *next)
{
	struct cpu_context *ctx = cpu_get_ctx();

	if (next->pml4_phys != 0) {
		// 目标是用户线程
		if (ctx->active_pml4_phys != next->pml4_phys) {
			// 只有物理地址真正改变时才刷 CR3
			arch_write_cr3((uint64_t)next->pml4_phys);
			ctx->active_pml4_phys = next->pml4_phys;
		}
	} else {
		// 目标是内核线程：保持当前的 active_pml4_phys 不变，也不切 CR3
		// 这样它就"借用"了前一个进程的内核映射部分
		if (thread_get_status(prev) == THREAD_ZOMBIE) {
			// 安全起见：切回纯净的内核页表，断开与已销毁进程的联系
			uint64_t k_pml4_phys = arch_kernel_root_phys();
			if (ctx->active_pml4_phys != (void*)k_pml4_phys) {
				arch_write_cr3(k_pml4_phys);
				ctx->active_pml4_phys = (void*)k_pml4_phys;
			}
		}
		// 如果 prev_thread 没退出，则继续"借用"当前 CR3
	}
}

void __switch_mm_classic(void)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *next = ctx->current;

	// 切换页表 (MMU 隔离)
	if (next->pml4_phys != 0) {
		L("CPU %d to user next %p %s %ld pml4 %p",
				ctx->id, next, next->name, next->id, next->pml4_phys);
		uint64_t current_cr3 = arch_read_cr3();
		if (current_cr3 != (uint64_t)next->pml4_phys) {
			// 如果是用户线程，加载它的私有页表物理地址
			arch_write_cr3((uint64_t)next->pml4_phys);
		}
	} else {
		// 内核线程不需要切换 CR3，它直接"借用"当前 CPU 上存在的上一个页表。
		// 因为所有页表的内核空间映射都是一样的。
		// 目标是内核线程：保持现状，什么都不做。
		// 如果是内核线程，确保使用的是内核 PML4 (注意需要物理地址)
		extern uint64_t *kernel_pml4;
		uint64_t k_pml4_phys = virt_to_phys(kernel_pml4);
		L("CPU %d to kernel next %p %s %ld pml4 %p", ctx->id,
				next, next->name, next->id, next->pml4_phys);
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

	/* O(1): bitmap scan for highest non-empty priority */
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

	/*
	 * The current RUNNING thread is deliberately skipped by the READY scan.
	 * Keep it running when every other runnable candidate has a strictly
	 * lower priority.  An equal-priority peer still wins here, which gives
	 * round-robin rotation without manufacturing a prev == next switch.
	 */
	if (prev != ctx->idle &&
			THREAD_RUNNING == thread_get_status(prev) &&
			(!next || prev->priority < found_prio)) {
		next = prev;
		found_prio = -1;
	}

	if (likely(next)) {
		if (next != prev) {
			/* Round-robin within priority: rotate to tail */
			list_del(&next->node);
			if (list_empty(&ctx->runqueue.heads[found_prio])) {
				ctx->runqueue.bitmap[0] &= ~(1ULL << found_prio);
			}
			list_add_tail(&next->node,
					&ctx->runqueue.heads[found_prio]);
			ctx->runqueue.bitmap[0] |= (1ULL << found_prio);
		}
	} else {
		// 否则切换到 idle 线程，如果 prev 已经是 idle，也会走这个流程
		// L("CPU %d (n)one idle %lu threads in rq", ctx->id, atomic64_read(&ctx->runqueue.count));
		/* 异常情形(队列非空但找不到 READY)下 dump 队列内容 */
#if CONFIG_RCU_DEBUG
		{
			struct thread *dpos = NULL;
			int idx = 0;
			for (int p = 0; p < SCHED_PRIO_COUNT && idx < 10; p++) {
				list_for_each_entry(dpos, &ctx->runqueue.heads[p], node) {
					L("  rq[%d] prio=%d %s(%ld) status=%d nest=%d blkd=%d",
							idx, p, dpos->name, dpos->id, thread_get_status(dpos),
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
	if (prev != next &&
			THREAD_RUNNING == thread_get_status(prev))
		thread_set_status(prev, THREAD_READY);
	prev->ticks++;

	/* 跟踪 pick_next 出口时 prev / next 的状态归属。
	 * 过滤范围：除 idle / reader (rcu-rd-*) / updater (rcu-upd) 外全部打印。
	 * 目的：验证"prev 卡 RUNNING"是不是状态写回路径漏写造成的。 */
#if CONFIG_RCU_DEBUG
	{
		const char *pn = prev->name;
		int skip = 0;
		if (prev == ctx->idle) {
			skip = 1;
		} else if (pn[0]=='r' && pn[1]=='c' && pn[2]=='u' && pn[3]=='-') {
			skip = 1;
		}
		if (!skip) {
			L("[PICK OUT] CPU %d prev=%s pst=%d next=%s nst=%d same=%d",
					ctx->id, prev->name, thread_get_status(prev),
					next->name, thread_get_status(next), (prev == next) ? 1 : 0);
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
		/* Clear bitmap if priority list now empty */
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

	L("CPU %d will switch to idle for zombie %p %s %ld", ctx->id, prev, prev->name, prev->id);

	return next;
}

__attribute__((noinline)) void __schedule(bool preemptive)
{
	// 关闭本地中断并保存原中断状态
	uint64_t flags = save_and_disable_interrupts();

	// 通过架构 per-CPU 机制获取当前 CPU 上下文
	struct cpu_context *ctx = cpu_get_ctx();

	/*
	 * 跟踪非 idle，非 reader，非 updater 的线程进 __schedule，
	 * 证实它们到底有没有进入 __schedule(区分"卡 RUNNING 不进调度"和"进了但
	 * 状态被改坏").
	 */
#if CONFIG_RCU_DEBUG
	{
		struct thread *p = ctx->current;
		const char *n = p->name;
		int skip = 0;
		if (p == ctx->idle) {
			skip = 1;
		} else if (n[0]=='r' && n[1]=='c' && n[2]=='u' && n[3]=='-') {
			skip = 1;
		}
		if (!skip) {
			L("[SCHED IN] CPU %d prev=%s status=%d preempt_cnt=%d preemptive=%d",
					ctx->id, n, thread_get_status(p), atomic_read(&ctx->preempt_count), preemptive);
		}
	}
#endif
	if (atomic_read(&ctx->preempt_count)) {
		if (preemptive) {
			L("Scheduling while atomic! count %d return", atomic_read(&ctx->preempt_count));
			restore_interrupts(flags); /* 入口处 save_and_disable_interrupts，必须恢复 */
			return;
		} else {
			// 但如果是主动调度(！preemptive)，要求 count 必须为 0
			panic("Scheduling while atomic! count %d", atomic_read(&ctx->preempt_count));
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
	 *   - 必须晚于 next 选定(prev 已确定要被切走)
	 *   - 必须早于 ctx->current = next(语义上 prev 仍是"当前线程")
	 *   - 必须早于 switch_mm / switch_to(切走前的最后记账机会)
	 *
	 * 行为：若 prev 仍在 RCU 临界区(nesting > 0)，把 prev 登记进全局 blocked_tasks，
	 * 否则视本次切换为本 CPU 的一次安静点， 更新 ctx->rcu_gp_seq_seen
	 */
	rcu_note_context_switch(prev);

	tss_set_rsp0(ctx->id, (uint64_t)next->kernel_stack);

	switch_mm(prev, next);

	/* 与 TTY9 采样共用 runqueue.lock，保证远端核读取 current、
	 * run_tsc 与 last_tsc 时得到同一代的状态。中断在本核已关闭，
	 * 这里的裸锁只会与远端 monitor 采样短暂竞争。 */
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

/**
 * __schedule_irq() - IRQ 即将返回时执行的调度。
 *
 * 调用点是 idt_stubs.S，在 ret_from_intr 路径里。
 * 这是 语义上这就是 "抢占式调度"(preemptive scheduling)的定义。
 */
inline void __schedule_irq(void)
{
	__schedule(true);
}

/**
 * __schedule_preempt() - 兑现 preempt_enable() 推迟下来的抢占请求。
 *
 * 注:cpu.c:preempt_enable 读 need_resched 与本函数进入 __schedule
 * 之间窗口开中断；窗口内若来 timer IRQ,ret_from_intr 会经由
 * __schedule_irq 自行进入 __schedule，信号由 IRQ 路径兜底，不丢失。
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
	// 无锁读取 g_ticks:与 BSP ++ 可能差 1 tick，仅影响截止时刻，不丢唤醒
	current->wakeup_ticks = atomic64_read(&g_ticks) + t;
	__schedule(false);

	// 唤醒后计算实际睡眠 tick 数(扣除 t 得调度延迟)
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
	struct thread *t = ctx->current;
	kprintf(" CPU %d ctx %p current %p %s %d running %ld threads idle(%p) ticks %lu\n",
			ctx->id, ctx, t, t->name, t->id, atomic64_read(&ctx->runqueue.count), ctx->idle, ctx->idle->ticks);

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&ctx->runqueue.lock, flags);

	struct thread *pos = NULL;
	int n = 0;
	for (int p = 0; p < SCHED_PRIO_COUNT; p++) {
		list_for_each_entry(pos, &ctx->runqueue.heads[p], node) {
			L("  #%d prio=%d %s(%ld) status %d ticks %lu cycles %lu sleeps %lu pml4_phys %p stack %p %p %s",
					n, p, pos->name, pos->id, thread_get_status(pos),
					pos->ticks, pos->run_tsc, pos->sleep_times, pos->pml4_phys,
					pos->kernel_stack, pos->user_stack, THREAD_STATUS_STR[pos->status]);
			n++;
		}
	}

	arch_spin_unlock_irqrestore(&ctx->runqueue.lock, flags);
}

void sched_snap(void)
{
	// 多核调度器快照
	kprintf("SMP %lu\n", g_cpu_count);
	// TODO RCU is needed?
	for (int i = 0; i < (int)g_cpu_count; i++) {
		sched_snap_cpu(i);
	}
}
