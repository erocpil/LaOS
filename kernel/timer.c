/*
 * timer.c - LAPIC 定时器中断处理
 *
 * timer_handler 处理架构定时器的 per-CPU 周期中断，驱动调度器 tick、
 * RCU 安静点记账、超时唤醒。
 */

#include "config.h"
#include "idt.h"
#include "cpu.h"
#include "timer.h"
#include "debug.h"
#include "rcu.h"
#include "export.h"
#include "selftest.h"

/**
 * timer_ticks -- 全局时钟滴答计数，仅在 BSP(CPU 0)累加。
 *
 * 设计意图：全局唯一时钟源。所有 CPU 的 LAPIC timer 同时触发，
 * 但仅 CPU 0 执行 ++，避免多写者竞争。其他 CPU 只读。
 * EXPORT_SYMBOL 供模块获取运行时间。
 */
atomic64_t timer_ticks = ATOMIC64_INIT(0);
EXPORT_SYMBOL(timer_ticks);

/**
 * g_ticks -- 调度器超时基准滴答数(BSP 单写，atomic64 多读).
 *
 * 生命周期：
 *   timer_handler (BSP)         atomic64_inc(&g_ticks)  // 唯一写点，每个 tick +1
 *   schedule_timeout (任意 CPU) wakeup_ticks = atomic64_read(&g_ticks) + t  // 计算唤醒截止
 *   pick_next (任意 CPU)        wakeup_ticks <= atomic64_read(&g_ticks)      // 判定是否超时
 *   schedule_timeout (任意 CPU) return atomic64_read(&g_ticks) - wakeup_ticks // 实际睡眠 tick 数
 *
 * 正确性分析：
 *   写安全：g_ticks 仅 BSP(CPU 0)在 timer_handler 中写入，单写者无竞争。
 *   读安全：atomic64_read(__ATOMIC_RELAXED)对 x86_64 TSO 等价于 volatile 裸读
 *     (单条 mov 无额外开销)，但提供跨架构移植的语义保证：ARM64 上自动获得
 *     正确屏障而无须修改调用方代码。
 *   类型：uint64_t，100Hz 下永不溢出(>500 年才到 2^32，2^64 量级为 10^9 年)。
 *
 * 为何选 atomic64_t 而非 volatile：
 *   - x86_64 上 atomic64_read RELAXED 与 volatile 裸读等价(同一条 mov)，
 *     无性能损失。
 *   - ARM64 弱内存模型上 volatile 不足以保证可见性，需显式 barrier；
 *     atomic64 由 GCC __atomic_* builtin 自动选择正确指令(LDAR/STLR 或 DMB)。
 *   - 写路径 atomic64_inc 在 x86_64 上为 lock inc(~20 cycles)，
 *     比 volatile ++(~1 cycle)略慢，但 timer_handler 每 10ms 才触发一次，
 *     完全不在热路径上，权衡正确性优先。
 *   - 统一接口：所有跨 CPU 共享变量用同一套 API，减少审阅负担和移植 bug。
 *
 * 与 timer_ticks 的关系：
 *   当前两者始终相等(同一 `if (0 == cpu_id)` 内 ++)，但语义独立：
 *     - timer_ticks: 全局时钟源，EXPORT_SYMBOL 供模块获取运行时长
 *     - g_ticks: 调度器本地 tick，未来可排除关中断期间流逝的滴答
 *                (如关中断期间不推进 g_ticks，避免唤醒判断偏差)
 */
atomic64_t g_ticks = ATOMIC64_INIT(0);

/**
 * timer_handler() - LAPIC 定时器中断处理(per-CPU 执行)。
 *
 * 每 CPU 的 LAPIC timer 独立触发，经 idt_handler -> irq_handler
 * 分发至此.BSP 负责推进全局计数器；所有 CPU 执行 need_resched
 * 置位，RCU 安静点记账，idle 统计等本地操作。
 */
void timer_handler(struct interrupt_frame *frame)
{
	(void)frame;

	uint32_t cpu_id = cpu_get_ctx()->id;

	// BSP: 推进全局计数器(单写，避免多 CPU 竞争)
	if (0 == cpu_id) {
		atomic64_inc(&timer_ticks);
		atomic64_inc(&g_ticks);
		arch_smp_probe_report();
		selftest_tick();

#if CONFIG_TIMER_DEBUG_PRINT
		if (atomic64_read(&timer_ticks) % (TIMER_HZ / 10) == 0) {
			print_at(1110, 0, 0xffffff, 0x000000,
					"%lu of %d Hz", atomic64_read(&timer_ticks), TIMER_HZ);
		}
#endif

#if CONFIG_TIMER_DEBUG_PRINT
		if (atomic64_read(&timer_ticks) % TIMER_HZ == 0) {
			uint64_t seconds = atomic64_read(&timer_ticks) / TIMER_HZ;
			print_at(1000, 20 * (cpu_id + 1), 0x00FFFF, 0x000000,
					"CPU %u UPTIME: %d SEC %lu",
					cpu_id, seconds, atomic64_read(&timer_ticks));
		}
#endif
	}

	// 每 TIMER_HZ/10 滴答(100ms)置 need_resched，触发抢占
	if (atomic64_read(&timer_ticks) % (TIMER_HZ / 10) == 0) {
		struct cpu_context *ctx = cpu_get_ctx();
		if (!ctx) {
			panic("NULL ctx");
		}
		ctx->need_resched = 1;
	}

	/*
	 * RCU 安静点记账：每次 tick 让本 CPU 检查是否处于 RCU 临界区。
	 * 若 nesting==0，把全局 gp_seq 复制到 per-CPU rcu_gp_seq_seen,
	 * synchronize_rcu 阶段一以此为推进依据。
	 *
	 * 在 EOI 之前调用:rcu_check_quiescent_state 仅做一次本 CPU
	 * 的 per-CPU 字段读写，不会触发新的中断处理，但放在 EOI 后会增加
	 * 极短的中断重入窗口，不必要。
	 */
	rcu_check_quiescent_state();
}
