/*
 * timer.c — LAPIC 定时器中断处理
 *
 * timer_handler 处理各 CPU 的周期中断，更新调度器时钟、RCU 安静点及超时状态。
 */

#include "config.h"
#include "idt.h"
#include "cpu.h"
#include "timer.h"
#include "debug.h"
#include "rcu.h"
#include "export.h"
#include "tests/selftest.h"

/*
 * timer_ticks 是全局时钟滴答计数，仅由 BSP（CPU 0）递增。
 *
 * 各 CPU 的 LAPIC 定时器均触发 timer_handler，但只有 BSP 更新此计数器。
 * 该符号导出给模块，用于读取系统运行时间。
 */
atomic64_t timer_ticks = ATOMIC64_INIT(0);
EXPORT_SYMBOL(timer_ticks);

/*
 * g_ticks 是调度器超时计算使用的滴答计数器，由 BSP 单写、各 CPU 原子读取。
 *
 * schedule_timeout() 以该计数器计算到期时间；调度器通过原子读取检查超时。
 * 计数器不用于发布其他共享数据，因此读取采用 relaxed 内存序即可。
 */
atomic64_t g_ticks = ATOMIC64_INIT(0);

/*
 * 处理单个 CPU 的 LAPIC 定时器中断。
 *
 * BSP 推进全局时钟；每个 CPU 更新本地重调度请求并记录 RCU 安静点。
 */
void timer_handler(struct interrupt_frame *frame)
{
	(void)frame;

	uint32_t cpu_id = cpu_get_ctx()->id;

	// 仅 BSP 推进全局时钟计数器。
	if (0 == cpu_id) {
		atomic64_inc(&timer_ticks);
		atomic64_inc(&g_ticks);
		arch_smp_probe_report();
		selftest_tick();

#if CONFIG_TIMER_DEBUG_PRINT
		if (atomic64_read(&timer_ticks) % (TIMER_HZ / 10) == 0) {
			print_at(1110, 0, 0xffffff, 0x000000, "%lu of %d Hz",
				atomic64_read(&timer_ticks), TIMER_HZ);
		}
#endif

#if CONFIG_TIMER_DEBUG_PRINT
		if (atomic64_read(&timer_ticks) % TIMER_HZ == 0) {
			uint64_t seconds = atomic64_read(&timer_ticks) / TIMER_HZ;
			print_at(1000, 20 * (cpu_id + 1), 0x00FFFF, 0x000000,
				"CPU %u UPTIME: %d SEC %lu", cpu_id, seconds,
				atomic64_read(&timer_ticks));
		}
#endif
	}

	// 每 100 ms 请求一次重调度。
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
	 * 此检查只访问本 CPU 的状态，不会触发新的中断处理；在中断结束前完成，
	 * 避免不必要地扩大中断重入窗口。
	 */
	rcu_check_quiescent_state();
}
