#ifndef __TTY_H__
#define __TTY_H__

/*
 * tty.h - TTY 控制台接口
 */

#include "lock.h"
#include "atomic.h"
#include <stdint.h>

#define MAX_TTYS 10
#define TTY_MONITOR 9
#define TTY_RCU 8
#define TTY_NET 7
#define TTY_CONF 6

/* 保守上限:覆盖 640×400 @ 8×16 字模(80×25) */
#define MAX_TTY_COLS 80
#define MAX_TTY_ROWS 25

/** TTY 字符网格单元:记录字符及其前景/背景色。 */
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
	// 当前在前台运行的进程 PID
	int current_pid;
	// 如果有输入队列
	char input_queue[64];
	int head;
	int tail;

	spinlock_t lock;
};

// 全局 TTY 数组和当前激活的 TTY 索引
extern struct tty ttys[MAX_TTYS];
extern atomic_t current_tty_id;

void tty_init(void);
int tty_ready(void);

/**
 * 切换到指定 TTY 并立即重绘其字符网格。
 *
 * 调用方(idt 键盘路径)已禁中断，无需内部持锁；
 * 调用方保证 new_id ∈ [0, MAX_TTYS).
 */
void tty_switch(int new_id);

/**
 * 记录一个字符到当前 TTY 的网格中。
 *
 * 由 draw_char() 在每次像素写入后调用。
 * 坐标 x, y 为像素坐标。
 */
void tty_record_char(int x, int y, char c, uint32_t fg, uint32_t bg);

/**
 * 通知 TTY 子系统当前屏幕已被清空(如 fb_clear_screen)。
 *
 * 遍历当前 TTY 网格，将所有 c 置为 ' ' 即空格。
 */
void tty_clear_grid(void);

#endif
