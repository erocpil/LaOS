/*
 * stats.c - 统计采集与 TTY 渲染
 *
 * 逐秒采集 CPU/RCU/PMM 运行数据，供应 TTY monitor 的各路页面。
 */
#include "stats.h"
#include "lock.h"
#include "pmm.h"
#include "timer.h"
#include "thread.h"
#include "rcu.h"
#include "task.h"
#include "task_conf.h"
#include "color.h"
#include "debug.h"
#include "export.h"

// =============================================================================
// LaOS System Monitor (TTY 9) : TSC 单口径 CPU 占用率说明
// =============================================================================
//
// 本文件渲染的 monitor 面板中，\"CPU CORES\" 行与线程 \"%CPU\" 列使用同一口径
// (TSC cycle 粒度)和同一个帧时间点，因此可比：idle.%CPU + 非 idle 线程.%CPU
// 之和 ≈ 100%，非 idle 线程 %CPU 之和 ≈ Core 行该核显示的占用率。
//
// 1) \"CPU CORES\" 行(核维度)
//    数据源:cpu_context.last_cpu_tsc + idle 线程的 run_tsc
//    公式：  (cpu_delta - idle_delta) / cpu_delta
//    语义：  该核在过去 1 秒内非 idle 线程的 TSC 占比，与下方线程明细同源。
//
// 2) \"%CPU\" 列(线程维度)
//    数据源:thread.run_tsc + thread.last_tsc + thread.last_snapshot_tsc
//    累加点:sched.c 切换离开时 prev->run_tsc += rdtsc() - prev->last_tsc;
//            正在 CPU 上跑，尚未切换的那一段以本帧 TSC 时间点补回。
//    公式：  (cur_run_tsc - last_snapshot) / (cur_cpu_tsc - last_cpu_tsc)
//    语义：  单个线程在过去 1 秒内独占该 CPU 的 cycle 比例，对应 Linux
//            top 的 %CPU 列口径。
//
// 两者的预期关系：
//   idle.%CPU + 非 idle 线程.%CPU 之和 ≈ 100%
//   非 idle 线程 %CPU 之和 ≈ \"CPU CORES\" 行该核显示的占用率
//
// runqueue 内容注释：
//   runqueue 装的是所有"归属本 CPU 且 status != ZOMBIE"的线程，包括
//   READY / RUNNING / SLEEPING / BLOCKED.pick_next 用 list_del + list_add_tail
//   只是把不可运行线程挪到队尾，并不出队。因此表格能列出所有活线程，
//   只是 SLEEPING/BLOCKED 线程在窗口内 %CPU 通常为 0.
// =============================================================================

/* 取一个线程在 sample_tsc 时刻的已运行 TSC 周期数。
 *
 * sched.c 仅在切换离开时累加 prev->run_tsc += rdtsc() - prev->last_tsc，
 * 因此一个正在 CPU 上运行的线程，其 run_tsc 漏算了"上次被调度上来到现在"
 * 这一段.stats 抽样窗口(1s)内若某线程恰好正在跑且未发生切换，直接读
 * run_tsc 会得到 0 增量，显示 0%:观测偏差。
 *
 * 当目标线程恰好等于其所在核的 current 时，补上 sample_tsc - last_tsc。
 * 调用者必须持有 c_ctx->runqueue.lock；调度器更新 current/run_tsc/last_tsc
 * 时持有同一把锁，故跨核采样不会读到切换中的混合状态。
 */
static inline uint64_t thread_run_tsc_at(struct thread *t, struct cpu_context *c_ctx,
		uint64_t sample_tsc)
{
	uint64_t base = t->run_tsc;

	if (c_ctx && c_ctx->current == t) {
		uint64_t last = t->last_tsc;
		if (sample_tsc > last) {
			base += sample_tsc - last;
		}
	}

	return base;
}

/* 按线程状态映射语义色： monitor 行整行一色，状态即色：
 *   RUNNING  -> OK    (青绿，活跃)
 *   READY    -> INFO  (蓝，候选)
 *   BLOCKED  -> ALERT (橙，告警，等资源)
 *   SLEEPING -> DIM   (灰，被动等待，降权)
 *   ZOMBIE   -> DIM   (灰，已退出，降权)
 *   EXITED   -> DIM   (灰)
 *
 * idle 行虽然 status == RUNNING，但占主导反而干扰阅读:idle 行另用 DIM.
 */
static inline uint32_t thread_status_color(int status)
{
	switch (status) {
		case THREAD_RUNNING:  return COLOR_OK;
		case THREAD_READY:    return COLOR_INFO;
		case THREAD_BLOCKED:  return COLOR_ALERT;
		case THREAD_SLEEPING: return COLOR_DIM;
		case THREAD_ZOMBIE:   return COLOR_DIM;
		case THREAD_EXITED:   return COLOR_DIM;
		default:              return COLOR_NORMAL;
	}
}

void stats_cpu(void)
{
	/* 全部 CPU 使用同一个 TSC 时间点。TSC 在本平台跨核同步，故该值可作
	 * 全局帧边界；逐核取样的锁等待不再改变统计窗口。 */
	uint64_t sample_tsc = rdtsc();
	uint64_t cpu_delta[MAX_CPUS] = { 0 };

	// 1. 清除所有内容
	fb_clear_screen(0);

	// 2. 算 UPTIME (转换为 标准时：分：秒 格式)
	uint64_t total_seconds = atomic64_read(&timer_ticks) / TIMER_HZ;
	uint64_t hour = total_seconds / 3600;
	uint64_t min = (total_seconds % 3600) / 60;
	uint64_t sec = total_seconds % 60;

	// 闪烁治理：渲染期间禁抢占，锁住整段 render.
	// 前置条件:sched.c 已修复抢占语义(check_need_schedule 在 preempt_count != 0
	// 时 return 0;__schedule_irq 传 preemptive=true),timer ISR 击中本段时
	// IRQ 返回路径会直接跳过调度，不会触发 "Scheduling while atomic".
	//
	// 收益：单核内 render 一半被切走，回来后表头/分隔线/数据行错位的视觉断裂
	// 概率降为 0；同时作为"抢占语义自洽性"的实战用例(kprintf 内部嵌套获取
	// print_lock 会把 preempt_count 推到 2 再退回 1，结尾 preempt_enable 把
	// 1 -> 0，最后一步如果有 need_resched 待处理，执行 __schedule_preempt 路径).
	preempt_disable();

	kprintf_color(COLOR_BG_HL, "=======================================================================================================================\n");
	kprintf_color(COLOR_WHITE, " LaOS System Monitor (TTY 9)                                                                          UPTIME: %02lu:%02lu:%02lu\n", hour, min, sec);
	kprintf_color(COLOR_BG_HL, "=======================================================================================================================\n");

	// 3. 规范化 PMM 内存 (1 Page = 4KB)
	uint64_t pmm_total = pinfo.page.total;
	uint64_t pmm_usable = pinfo.page.usable;
	uint64_t pmm_freed = pinfo.page.freed;
	// 防御：理论上 freed <= usable，但若分配/释放计数错位导致 freed > usable
	// 直接相减会下溢出 uint64_t 得到极大值。这里钳到 0.
	uint64_t pmm_used = (pmm_freed <= pmm_usable) ? (pmm_usable - pmm_freed) : 0;

	// 将页数转换为 MB 展现
	uint64_t total_mb = (pmm_total * 4096) / 1024 / 1024;
	uint64_t usable_mb = (pmm_usable * 4096) / 1024 / 1024;
	uint64_t used_mb = (pmm_used * 4096) / 1024 / 1024;
	// 与 %CPU 一致，放大 10000 倍取两位小数
	int mem_used_pct = pmm_usable ? (int)((pmm_used * 10000) / pmm_usable) : 0;

	uint32_t mem_color = (mem_used_pct < 5000) ? COLOR_OK
		: (mem_used_pct < 8000) ? COLOR_WARN : COLOR_ALERT;
	kprintf_color(mem_color,
			" PMM MEMORY: Total: %lu MB (%lu Pages) | Usable: %lu MB | Used: %lu MB (%d.%02d%%)\n",
			total_mb, pmm_total, usable_mb, used_mb, mem_used_pct / 100, mem_used_pct % 100);

	// 4. 打印每个 CPU 核心的非 idle 占比(TSC 粒度，与下方线程明细同口径).
	// 用各核 idle 线程的 TSC 增量反推：非 idle% = (total_tsc - idle_tsc) / total_tsc.
	kprintf_color(COLOR_INFO, " CPU CORES:  ");
	for (uint64_t i = 0; i < g_cpu_count; i++) {
		struct cpu_context *c_ctx = g_cpu_contexts[i];
		if (!c_ctx) {
			kprintf("[Core %lu: ------%%]  ", i);
			continue;
		}

		uint64_t flags = 0;
		arch_spin_lock_irqsave(&c_ctx->runqueue.lock, flags);
		cpu_delta[i] = sample_tsc - c_ctx->last_cpu_tsc;

		/* idle 线程快照：补上"正在跑还没结算"的本帧尾段。 */
		uint64_t idle_live = thread_run_tsc_at(c_ctx->idle, c_ctx, sample_tsc);
		uint64_t idle_delta = 0;
		if (c_ctx->last_idle_tsc != 0 && idle_live >= c_ctx->last_idle_tsc) {
			idle_delta = idle_live - c_ctx->last_idle_tsc;
		}
		c_ctx->last_idle_tsc = idle_live;

		/* 首帧打 ------% */
		if (c_ctx->last_cpu_tsc == 0 || cpu_delta[i] == 0) {
			kprintf_color(COLOR_DIM, "[Core %lu: ------%%]  ", i);
		} else {
			if (idle_delta > cpu_delta[i]) idle_delta = cpu_delta[i];
			int rate = (int)(((cpu_delta[i] - idle_delta) * 10000) / cpu_delta[i]);
			uint32_t color = (rate < 5000) ? COLOR_OK
				: (rate < 8000) ? COLOR_WARN : COLOR_ALERT;
			kprintf_color(color, "[Core %lu: %3d.%02d%%]  ", i,
				rate / 100, rate % 100);
		}

		c_ctx->last_cpu_tsc = sample_tsc;
		arch_spin_unlock_irqrestore(&c_ctx->runqueue.lock, flags);
	}
	kprintf("\n");

	// 5. 表头(固定列宽，列间 2 空格分隔，总宽 107)
	// CORE(4) PID(4) NAME(13) STATUS(6) %CPU(7) TICKS(8) TSC_CYCLES(14) SLEEPS(8) PREEMPTS(9) PML4_CR3(18)
	// 与上方 "CPU CORES" 行均为同一帧的 TSC 粒度统计；详见文件顶部说明。
	kprintf_color(COLOR_BG_HL, "-----------------------------------------------------------------------------------------------------------------------\n");
	kprintf_color(COLOR_INFO, " THREAD DETAIL (per-thread, TSC cycle granularity)\n");
	kprintf_color(COLOR_DIM, " CORE   PID  NAME          STATS      %%CPU         TICKS          TSC_CYCLES    SLEEPS   PREEMPTS   PML4_CR3\n");
	kprintf_color(COLOR_BG_HL, "-----------------------------------------------------------------------------------------------------------------------\n");

	// 6. 遍历所有 CPU 的 runqueue
	for (int i = 0; i < (int)g_cpu_count; i++) {
		struct cpu_context *c_ctx = g_cpu_contexts[i];
		if (!c_ctx) {
			continue;
		}

		// 本核窗口已在 Core 行用 sample_tsc 固定；线程行复用同一分母。
		uint64_t cpu_total_delta = cpu_delta[i];

		if (cpu_total_delta == 0) {
			cpu_total_delta = 1;
		}

		// 同一把锁也保护调度器对 current/run_tsc/last_tsc 的更新。
		struct thread *idle = c_ctx->idle;
		uint64_t flags = 0;
		arch_spin_lock_irqsave(&c_ctx->runqueue.lock, flags);

		// 算出该线程在这一秒内，实际在 CPU 上跑了多少周期
		// 用固定帧时间点补上"正在 CPU 上跑，还没结算"的那一段。
		uint64_t cur_thread_tsc = thread_run_tsc_at(idle, c_ctx, sample_tsc);
		uint64_t thread_delta = 0;

		// last_snapshot_tsc == 0 表示首次抽样，未初始化的快照不能用作 delta 基准，
		// 否则 delta = 整段 run_tsc，触发 100% 误显示。本帧记 0%，下帧起正常。
		if (idle->last_snapshot_tsc != 0 && cur_thread_tsc >= idle->last_snapshot_tsc) {
			thread_delta = cur_thread_tsc - idle->last_snapshot_tsc;
		}

		// 更新线程自身的快照，留给下一秒
		idle->last_snapshot_tsc = cur_thread_tsc;

		// 计算百分比:(线程周期 / 核心总周期) * 100
		// 为了避免内核浮点运算，放大 100 倍计算
		int usage = (int)((thread_delta * 10000) / cpu_total_delta);
		if (usage > 10000) {
			usage = 10000;
		}

		int status_val = thread_get_status(idle);
		uint32_t row_color = thread_status_color(status_val);
		kprintf_color(row_color,
				" [%2d]  %4d  %-13s %-6s  %3d.%02d%%  %12lu  %18lu  %8lu  %9u   %p\n",
				i,
				idle->id,
				idle->name,
				THREAD_STATUS_STR[status_val],
				usage / 100, usage % 100,
				idle->ticks,
				idle->run_tsc,
				idle->sleep_times,
				idle->preempts,
				(void*)idle->pml4_phys);

		struct thread *pos = NULL;
		for (int p = 0; p < SCHED_PRIO_COUNT; p++) {
			list_for_each_entry_reverse(pos, &c_ctx->runqueue.heads[p], node) {
				// 规范化状态可读性
				int status_val = thread_get_status(pos);
				const char *status_str = THREAD_STATUS_STR[status_val];

				// 算出该线程在这一秒内，实际在 CPU 上跑了多少周期
				// 用固定帧时间点补上"正在 CPU 上跑，还没结算"的那一段。
				uint64_t cur_thread_tsc = thread_run_tsc_at(pos, c_ctx, sample_tsc);
				uint64_t thread_delta = 0;

				// last_snapshot_tsc == 0:首次抽样，无 delta 基准(详见 idle 行注释)
				if (pos->last_snapshot_tsc != 0 && cur_thread_tsc >= pos->last_snapshot_tsc) {
					thread_delta = cur_thread_tsc - pos->last_snapshot_tsc;
				}

				// 更新线程自身的快照，留给下一秒
				pos->last_snapshot_tsc = cur_thread_tsc;

				// 计算百分比:(线程周期 / 核心总周期) * 100
				// 为了避免内核浮点运算，放大 100 倍计算
				int usage = (int)((thread_delta * 10000) / cpu_total_delta);
				if (usage > 10000) {
					usage = 10000;
				}

				// 将计算结果缓存在 TCB 中
				pos->last_cpu_usage = usage;

				// 列宽与 idle 行严格对齐，CORE 列用 4 空格占位。
				// 整行按线程状态着色:RUNNING/READY/BLOCKED 三态在演示视频里
				// 0.5s 内即可分类:RCU 临界区里被切走的 reader 行(仍 RUNNING // 状态)
				//   保持 OK 色，下方 RCU monitor 再用 nesting>0 高亮 in-CR.
				uint32_t row_color = thread_status_color(status_val);
				kprintf_color(row_color,
					"       %4d  %-13s %-6s  %3d.%02d%%  %12lu  %18lu  %8lu  %9u   %p\n",
					pos->id,
					pos->name,
					status_str,
					usage / 100, usage % 100,
					pos->ticks,
					pos->run_tsc,
					pos->sleep_times,
					pos->preempts,
					(void*)pos->pml4_phys);
			}
		}
		arch_spin_unlock_irqrestore(&c_ctx->runqueue.lock, flags);
	}

	kprintf_color(COLOR_BG_HL, "=======================================================================================================================\n");

	preempt_enable();
}

void stats_rcu(void)
{
	fb_clear_screen(0);
	preempt_disable();

	static uint32_t all_done = 0;

	rcu_metric.frame_no++;
	uint64_t uptime_s = atomic64_read(&timer_ticks) / TIMER_HZ;

	kprintf_color(COLOR_BG_HL, "=== ");
	kprintf_color(COLOR_RCU, "RCU Live (tty8)");
	kprintf_color(COLOR_SCHED, "  frame %lu  uptime %lus", rcu_metric.frame_no, uptime_s);
	kprintf_color(COLOR_BG_HL, "  =============================\n\n");

	if (all_done) {
		kprintf_color(COLOR_OK, " [ALL DONE]\n\n");
	}

	kprintf_color(COLOR_RCU, " instances: %u    global gp_seq: %u    gp_sync_times: %u\n",
			rcu_metric.n_rcu, rcu_metric.gp_seq, rcu_metric.gp_sync_times);
	kprintf_color(COLOR_RCU, " last_gp_cycles: %lu\n\n", rcu_metric.last_gp_cycles);

	uint32_t i = 0;
	uint32_t c = 0;
	struct rcu_instance_metric *pos = NULL;
	list_for_each_entry(pos, &rcu_metric.head, node) {
		const char *state = "RUN ";
		if (!pos->readers_alive) {
			state = "DONE";
			c++;
		}
		kprintf_color(COLOR_RCU, " [%u %s]  %s  readers %u / alive %u\n",
				i, pos->name, state, pos->n_readers, pos->readers_alive);
		for (uint32_t j = 0; j < pos->n_readers; j++) {
			uint64_t delta = pos->iters[j] - pos->iters_prev[j];
			kprintf_color(COLOR_OK, "   cpu%u iters %-10lu +%lu/s\n",
					j, pos->iters[j], delta);
			pos->iters_prev[j] = pos->iters[j];
		}
		kprintf_color(COLOR_RCU, "\n");
		i++;
	}

	all_done = c == rcu_metric.n_rcu;

	kprintf_color(COLOR_CPU, " per-CPU seen (gp_seq):\n   ");
	uint32_t n_behind = 0;
	for (uint32_t j = 0; j < g_cpu_count; j++) {
		struct cpu_context *ctx = g_cpu_contexts[j];
		if (!ctx) continue;
		int behind = ((uint32_t)ctx->rcu_gp_seq_seen < rcu_metric.gp_seq);
		const char *mark = behind ? "*" : " ";
		if (behind) {
			n_behind++;
		}
		kprintf_color(COLOR_RCU, "cpu%u %u%s  ", j, ctx->rcu_gp_seq_seen, mark);
	}
	if (n_behind > 0) {
		kprintf_color(COLOR_SCHED, "\n       (* = behind global, waiting for quiescent)\n");
	} else {
		kprintf_color(COLOR_SCHED, "\n");
	}

	kprintf_color(COLOR_BG_HL, "\n=======================================================================\n");

	preempt_enable();
}

/*
 * net stats 指针，由 module 加载完成后调 stats_net_register 交进来。
 * 无锁：单 writer(load)+ 单 reader(monitor thread)，指针赋值天然原子。
 * 上一帧快照用于算 packets/s,bytes/s 之类的 rate.
 */
static const struct e1000_stats *g_net_stats;
static struct e1000_stats g_net_prev; /* 全零起步，第一次 delta 就是本帧累计值 */
static uint64_t g_net_frame_no;

void stats_net_register(const struct e1000_stats *p)
{
	g_net_stats = p;
}
EXPORT_SYMBOL(stats_net_register);

void stats_net(void)
{
	fb_clear_screen(0);
	preempt_disable();

	const struct e1000_stats *s = g_net_stats;
	uint64_t uptime_s = atomic64_read(&timer_ticks) / TIMER_HZ;

	g_net_frame_no++;

	/* ── Title bar ── */
	kprintf_color(COLOR_BG_HL, "=== ");
	kprintf_color(COLOR_NET,    "NET Live (tty7)");
	kprintf_color(COLOR_DIM,    "  frame %lu  uptime %lus", g_net_frame_no, uptime_s);
	kprintf_color(COLOR_BG_HL, "  ==================================\n");

	if (!s) {
		kprintf_color(COLOR_DIM, " e1000 module not registered\n");
		preempt_enable();
		return;
	}

	/* ── Mode ── */
	static const char *rx_mode_names[] = {"?","simple","batch","batch_swap","single_thr","mt"};
	const char *mode_str = (s->rx_mode >= 1 && s->rx_mode <= 5) ? rx_mode_names[s->rx_mode] : "?";
	kprintf_color(COLOR_INFO, "  rx_mode %d (%s)   idle %s\n",
		s->rx_mode, mode_str,
		s->idle_mode ? "sleep" : "spin");

	/* ── Link ── */
	kprintf_color(s->link_up ? COLOR_OK : COLOR_ALERT,
		"  Link  %s   %u Mbps   MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
		s->link_up ? "UP  " : "DOWN", s->link_speed,
		s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5]);

	/* ── RX ── */
	uint64_t d_rx_pkts = s->rx_packets - g_net_prev.rx_packets;
	uint64_t d_rx_bytes = s->rx_bytes - g_net_prev.rx_bytes;

	kprintf_color(COLOR_BG_HL, "\n--- RX ----------------------------------------------------\n");
	kprintf_color(COLOR_OK, "  packets %-12lu (+%lu/s)    bytes %-12lu (+%lu/s)\n",
		s->rx_packets, d_rx_pkts, s->rx_bytes, d_rx_bytes);
	kprintf_color(COLOR_OK, "  processed %-10lu   dropped_qfull %-4lu   poolempty %lu\n",
		s->rx_processed, s->rx_dropped_qfull, s->rx_dropped_poolempty);

	/* ── TX ── */
	uint64_t d_tx_pkts = s->tx_packets - g_net_prev.tx_packets;
	uint64_t d_tx_bytes = s->tx_bytes - g_net_prev.tx_bytes;

	kprintf_color(COLOR_BG_HL, "\n--- TX ----------------------------------------------------\n");
	kprintf_color(COLOR_INFO, "  packets %-12lu (+%lu/s)    bytes %-12lu (+%lu/s)\n",
		s->tx_packets, d_tx_pkts, s->tx_bytes, d_tx_bytes);
	kprintf_color(COLOR_INFO, "  dd_timeout %lu\n", s->tx_dd_timeout);

	/* ── L2/L3 ── */
	kprintf_color(COLOR_BG_HL, "\n--- Protocol ----------------------------------------------\n");
	kprintf_color(COLOR_INFO, "  ARP   rx %-8lu  replies %lu\n",
		s->arp_rx, s->arp_replies);
	kprintf_color(COLOR_INFO, "  ICMP  echo_rx %-4lu  echo_reply %lu\n",
		s->icmp_echo_rx, s->icmp_echo_reply);
	kprintf_color(s->rx_dropped_unknown_ether || s->rx_dropped_unknown_proto
		? COLOR_ALERT : COLOR_DIM,
		"  unknown  ether %-4lu  proto %lu\n",
		s->rx_dropped_unknown_ether, s->rx_dropped_unknown_proto);

	/* ── Rings ── */
	kprintf_color(COLOR_BG_HL, "\n--- Rings -------------------------------------------------\n");
	kprintf_color(COLOR_DIM, "  RX ring  head %3u  tail %3u       rx_queue %3u  hwm %3u\n",
		s->rx_ring_head, s->rx_ring_tail,
		s->rx_queue_count, s->rx_queue_hwm);
	kprintf_color(COLOR_DIM, "  TX ring  head %3u  tail %3u       free_pool %3u\n",
		s->tx_ring_head, s->tx_ring_tail,
		s->free_pool_count);

	/* ── HW counters ── */
	uint64_t d_gprc = s->hw_gprc - g_net_prev.hw_gprc;
	uint64_t d_gptc = s->hw_gptc - g_net_prev.hw_gptc;
	uint64_t d_gorc = s->hw_gorc - g_net_prev.hw_gorc;
	uint64_t d_gotc = s->hw_gotc - g_net_prev.hw_gotc;

	kprintf_color(COLOR_BG_HL, "\n--- HW Counters -------------------------------------------\n");
	kprintf_color(COLOR_DIM, "  GPRC %-12lu (+%lu/s)    GORC %-12lu (+%lu/s)\n",
		s->hw_gprc, d_gprc, s->hw_gorc, d_gorc);
	kprintf_color(COLOR_DIM, "  GPTC %-12lu (+%lu/s)    GOTC %-12lu (+%lu/s)\n",
		s->hw_gptc, d_gptc, s->hw_gotc, d_gotc);
	kprintf_color(COLOR_DIM, "  TPR  %-12lu   TPT  %-12lu\n",
		s->hw_tpr, s->hw_tpt);
	kprintf_color(s->hw_crcerrs || s->hw_mpc || s->hw_rnbc || s->hw_colc
		? COLOR_ALERT : COLOR_DIM,
		"  CRCERRS %lu  MPC %lu  RNBC %lu  COLC %lu\n",
		s->hw_crcerrs, s->hw_mpc, s->hw_rnbc, s->hw_colc);

	/* 保存本帧快照供下帧算 delta */
	g_net_prev = *s;

	kprintf_color(COLOR_BG_HL, "\n============================================================\n");

	preempt_enable();
}

/** stats_conf() — TTY 6 任务配置全景视图。
 *
 * 完整展示 task.conf 解析结果：版本号、缺失模块策略、@test 测试编排、
 * 以及按 CPU 分组的任务清单（含位置参数和 kv 参数）。 */
void stats_conf(void)
{
	fb_clear_screen(0);
	preempt_disable();

	kprintf_color(COLOR_BG_HL, "=== ");
	kprintf_color(COLOR_NOTICE, "Task Configuration (tty6)");
	kprintf_color(COLOR_BG_HL, " ==========================================\n\n");

	/* ── 元信息 ── */
	kprintf_color(COLOR_MODULE, "  Version:  ");
	kprintf_color(COLOR_NORMAL, "1\n");
	kprintf_color(COLOR_MODULE, "  Strategy: ");
	kprintf_color(COLOR_NORMAL, "%s (missing modules are %s)\n\n",
			task_conf.module_missing_panic ? "panic" : "skip",
			task_conf.module_missing_panic ? "fatal" : "skipped");

	/* ── @test 指令 ── */
	const struct list_node *dhead = task_conf_get_directives();
	if (!list_empty(dhead)) {
		kprintf_color(COLOR_BG_HL, "  @test directives\n");
		kprintf_color(COLOR_MODULE, "  %-22s", "name");
		kprintf_color(COLOR_BG_HL, "parameters\n");
		kprintf_color(COLOR_BG_HL, "  %-22s%s\n", "--------------------", "----------");

		struct selftest_directive *d = NULL;
		list_for_each_entry(d, dhead, node) {
			kprintf_color(COLOR_MODULE, "  %-22s", d->name);
			if (d->kv_count > 0) {
				for (int i = 0; i < d->kv_count; i++) {
					if (i > 0)
						kprintf(" ");
					kprintf_color(COLOR_NORMAL, "%s=%s",
							d->kvs[i].key, d->kvs[i].value);
				}
			} else {
				kprintf_color(COLOR_NORMAL, "(no params)");
			}
			kprintf("\n");
		}
		kprintf("\n");
	}

	/* ── 任务清单表头 ── */
	kprintf_color(COLOR_BG_HL, "  Task entries\n");
	kprintf_color(COLOR_MODULE, " CPU   MODULE            NAME       TYPE      MAGIC          ARGS     KV\n");
	kprintf_color(COLOR_BG_HL, " ---   ------            ----       ----      -----          ----     --\n");

	int32_t cpu_id = -1;
	struct task *t = NULL;
	const char *type_names[] = {
		[TASK_KERNEL] = "kernel",
		[TASK_DRIVER] = "driver",
		[TASK_THREAD] = "thread",
		[TASK_USER]   = "user",
		[TASK_DATA]   = "data",
		[TASK_CONFIG] = "config",
	};

	list_for_each_entry(t, &task_conf.head, node) {
		if (t->cpu_id != cpu_id) {
			kprintf_color(COLOR_MODULE, "  %-4d ", t->cpu_id);
			cpu_id = t->cpu_id;
		} else {
			kprintf_color(COLOR_NORMAL, "       ");
		}

		const char *tname = (t->type < TASK_MAX) ? type_names[t->type] : "?";

		kprintf_color(COLOR_PMM, "%-16s%-10s%-8s%16p  ",
				t->module, t->name, tname, (void*)t->magic);

		if (t->args_buf[0])
			kprintf_color(COLOR_NORMAL, "%-8s", t->args_buf);
		else
			kprintf_color(COLOR_NORMAL, "%-8s", "-");

		kprintf_color(COLOR_NORMAL, " ");
		if (t->kv_count > 0) {
			for (int i = 0; i < t->kv_count; i++) {
				if (i > 0)
					kprintf(" ");
				kprintf("%s=%s", t->kv_keys[i], t->kv_values[i]);
			}
		} else {
			kprintf("-");
		}

		kprintf("\n");
	}

	kprintf_color(COLOR_BG_HL, "\n======================================================================\n");

	preempt_enable();
}

/** stats_sys() — TTY 0 系统概览，entry-only 渲染（不自动刷新）。
 *
 * 显示：uptime、CPU 核数、内存统计、已加载任务清单。
 * 不调用 fb_clear_screen，保留启动阶段可能残留的 boot log。
 */
void stats_sys(void)
{
	preempt_disable();

	uint64_t uptime_s = atomic64_read(&timer_ticks) / TIMER_HZ;

	kprintf_color(COLOR_BG_HL, "\n=== LaOS System ===\n\n");
	kprintf("  Uptime    %lu s\n", uptime_s);
	kprintf("  CPU cores %lu\n", g_cpu_count);

	/* PMM 内存统计 */
	uint64_t used = pinfo.page.usable - pinfo.page.freed;
	kprintf("  Memory    %lu MB total  %lu MB used  %lu MB free\n",
		pinfo.page.usable / 256, used / 256, pinfo.page.freed / 256);

	/* 任务清单 */
	kprintf_color(COLOR_BG_HL, "\n  Loaded tasks:\n");
	struct task *t = NULL;
	list_for_each_entry(t, &task_conf.head, node) {
		const char *type_names[] = {
			[TASK_KERNEL] = "kernel", [TASK_DRIVER] = "driver",
			[TASK_THREAD] = "thread", [TASK_USER]   = "user",
			[TASK_DATA]   = "data",   [TASK_CONFIG] = "config",
		};
		const char *tname = (t->type < TASK_MAX) ? type_names[t->type] : "?";
		kprintf("    cpu %d  %-8s  %s\n", t->cpu_id, tname, t->module);
	}

	kprintf("\n");
	preempt_enable();
}
