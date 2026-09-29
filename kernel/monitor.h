#ifndef __MONITOR_H__
#define __MONITOR_H__

/*
 * monitor.h - TTY monitor 统计页面
 */

#include <stdbool.h>

void start_monitor(void);

/*
 * monitor_ready — 标记 monitor 线程是否已创建。
 * idt.c 的 Alt-6/7/8/9 快捷键需检查此标记：
 * monitor 未就绪时切换过去会导致黑屏(无渲染线程),
 * 表现为系统卡死。见 commit cd5506a 上下文。
 */
extern bool monitor_ready;

#endif
