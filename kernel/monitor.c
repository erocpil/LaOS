/*
 * monitor.c - 多路 TTY monitor:CPU/PMM/RCU/NET 等统计页面
 */

#include <stdint.h>

#include "monitor.h"
#include "sched.h"
#include "timer.h"
#include "thread.h"
#include "debug.h"
#include "stats.h"
#include "tty.h"
#include "log.h"

bool monitor_ready = false;

// timer_ticks 通过 timer.h 的 extern atomic64_t 声明引入

static void monitor_func(void *data)
{
	monitor_ready = true; /* 线程真正开始运行时才标记就绪 */

	struct thread *t = get_current();
	t->tty_id |= 1 << 0; /* TTY 0: 默认终端，所有线程均可输出 */
	t->tty_id |= 1 << 7;
	t->tty_id |= 1 << 8;
	t->tty_id |= 1 << 9;
	t->tty_id |= 1 << 6;

	(void)data;

	/*
	 * 触发条件用"上次渲染至今相隔的 tick 数"判定，而不是 timer_ticks % TIMER_HZ == 0。
	 * 原版有两个问题：
	 *  1) 在 schedule_timeout(1) 唤醒粒度下，timer_ticks 不一定恰好落在边界值，
	 *     可能漏帧或一秒内打两帧。
	 *  2) monitor 切到 TTY_MONITOR 之外再切回时，第一帧需要等下一秒整边界，
	 *     用户感知"卡 1 秒不刷新"。
	 * 新条件：第一次进 TTY_MONITOR 立即出帧，之后保证两帧间隔 >= 1 秒。
	 */
	uint64_t last_render = 0;
	static int32_t last_tty_id = -1;

	while (1) {
		/* TTY 6-9 是 monitor 的专用显示通道，仅当激活 TTY 匹配
		 * monitor 持有的 bitmask 时才渲染统计面板。
		 * 其他 TTY(如 TTY 0 默认终端)上 monitor 休眠等待切换。
		 */
		int cur_tty = atomic_read(&current_tty_id);
		if ((1 << cur_tty) & t->tty_id) {
			switch (cur_tty) {
				case 0:
					if (last_tty_id != cur_tty) {
						stats_sys();
					}
					last_tty_id = cur_tty;
					schedule_timeout(50); /* 静态页面，低频轮询 */
					continue;
				case 6:
					if (last_tty_id != cur_tty) {
						stats_conf();
					}
					last_tty_id = cur_tty;
					schedule_timeout(50);
					continue;
				case 7: {
					uint64_t now = atomic64_read(&timer_ticks);
					if (last_render == 0 || now - last_render >= TIMER_HZ / 2) {
						stats_net();
						last_render = now;
					}
					break;
				}
				case 8: {
					uint64_t now = atomic64_read(&timer_ticks);
					if (last_render == 0 || now - last_render >= TIMER_HZ / 2) {
						stats_rcu();
						last_render = now;
					}
					break;
				}
				case 9: {
					uint64_t now = atomic64_read(&timer_ticks);
					if (last_render == 0 || now - last_render >= TIMER_HZ) {
						stats_cpu();
						last_render = now;
					}
					break;
				}
				default:
					schedule_timeout(1);
					continue;
			}
			last_tty_id = cur_tty;
			schedule_timeout(1);
		} else {
			// monitor 在当前 TTY 上不可见，短休眠等待切换
			last_render = 0;
			schedule_timeout(1);
		}
	}
}

void start_monitor(void)
{
	struct thread *t = thread_create_on(monitor_func, NULL, g_cpu_count - 1);
	thread_set_name(t, "monitor");
	cpu_enqueue(-1, t);
	L_TAG(LOG_TTY, "Monitor started.\n");
}
