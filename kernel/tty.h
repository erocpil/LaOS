#ifndef __TTY_H__
#define __TTY_H__

/*
 * tty.h - TTY 控制台接口
 */

#include "lock.h"
#include "atomic.h"
#include <stdint.h>

#define MAX_TTYS    10
#define TTY_MONITOR 9
#define TTY_RCU	    8
#define TTY_NET	    7
#define TTY_CONF    6

/* 按 640×400 分辨率和 8×16 字模设置容量上限（80×25 字符）。 */
#define MAX_TTY_COLS 80
#define MAX_TTY_ROWS 25

/* TTY 字符网格单元，记录字符及其前景色和背景色。 */
struct tty_cell {
	char c;
	uint32_t fg;
	uint32_t bg;
};

struct tty {
	int id;
	int cols; /* 实际列数 = fb_width / CHAR_WIDTH */
	int rows; /* 实际行数 = fb_height / CHAR_HEIGHT */
	struct tty_cell grid[MAX_TTY_COLS * MAX_TTY_ROWS];
	int cursor_x;
	int cursor_y;
	// 当前前台进程的 PID。
	int current_pid;
	// 输入队列及其读写位置。
	char input_queue[64];
	int head;
	int tail;

	spinlock_t lock;
};

/* 全局 TTY 数组及当前激活的 TTY 索引。 */
extern struct tty ttys[MAX_TTYS];
extern atomic_t current_tty_id;

void tty_init(void);
int tty_ready(void);

/*
 * 切换到指定 TTY 并立即重绘其字符网格。
 *
 * 调用方须在中断关闭时调用，并保证 new_id 位于 [0, MAX_TTYS) 范围内。
 */
void tty_switch(int new_id);

/*
 * 记录一个字符到当前 TTY 的网格中。
 *
 * draw_char() 在绘制字符后调用。x、y 使用像素坐标。
 */
void tty_record_char(int x, int y, char c, uint32_t fg, uint32_t bg);

/*
 * 通知 TTY 子系统当前屏幕已被清空(如 fb_clear_screen)。
 *
 * 将当前 TTY 网格中的字符重置为空格。
 */
void tty_clear_grid(void);

#endif
