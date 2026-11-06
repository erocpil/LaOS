#ifndef __MONITOR_H__
#define __MONITOR_H__

/*
 * monitor.h - TTY monitor 统计页面
 */

#include <stdbool.h>

void start_monitor(void);

/*
 * monitor_ready 表示监控线程已开始运行。键盘快捷键在切换到监控终端前
 * 检查此状态，避免在线程尚未运行时进入无人刷新的页面。
 */
extern bool monitor_ready;

#endif
