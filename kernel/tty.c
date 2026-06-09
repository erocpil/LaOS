/*
 * tty.c - TTY 控制台渲染引擎
 *
 * 多路虚拟终端(10 路，索引 0-9)的 framebuffer 渲染与字符网格 save/restore。
 * 每个 TTY 维护一个 MAX_TTY_COLS × MAX_TTY_ROWS 的 tty_cell 网格，
 * draw_char() 写入 fb 的同时记录到网格；切 TTY 时从网格重绘。
 */

#include <limine.h>

#include "tty.h"
#include "cpu.h"
#include "printf.h"
#include "log.h"

// 全局 TTY 数组和当前激活的 TTY 索引
struct tty ttys[MAX_TTYS];
atomic_t current_tty_id = { -1 };

void tty_init(void)
{
	struct limine_framebuffer *fb = fb_get_info();
	int cols = 0, rows = 0;

	if (fb) {
		cols = (int)fb->width / 8; /* CHAR_WIDTH */
		rows = (int)fb->height / 16; /* CHAR_HEIGHT */
		if (cols > MAX_TTY_COLS) {
			cols = MAX_TTY_COLS;
		}
		if (rows > MAX_TTY_ROWS) {
			rows = MAX_TTY_ROWS;
		}
	}

	for (int i = 0; i < MAX_TTYS; i++) {
		ttys[i].cols = cols;
		ttys[i].rows = rows;
	}

	atomic_set(&current_tty_id, 0);
	L_TAG(LOG_TTY, "tty multiplexer ready (%d×%d grid).\n", cols, rows);
}

/**
 * tty_ready() - 当前线程是否有权在当前 TTY 上输出。
 *
 * TTY 0 是默认终端，所有线程均可输出。
 * TTY 6-9 是专用通道，仅持有对应 tty_id 位的线程可输出(monitor
 * 线程设置 bits 6-9，见 monitor.c)。
 *
 * 返回:1 可输出，0 不可输出(kprintf 应跳过 fb 渲染，仅走串口)。
 */
int tty_ready(void)
{
	if (atomic_read(&current_tty_id) == -1) {
		return 0; // uninitialized
	}

	// TTY 0 is the default: any thread may write to it.
	if (atomic_read(&current_tty_id) == 0) {
		return 1;
	}

	// TTYs 6-9: thread must hold the matching tty_id bit.
	if ((1 << atomic_read(&current_tty_id)) & cpu_get_ctx()->current->tty_id) {
		return 1;
	}

	return 0;
}

void tty_record_char(int x, int y, char c, uint32_t fg, uint32_t bg)
{
	int cur = atomic_read(&current_tty_id);
	if (cur < 0 || cur >= MAX_TTYS) {
		return;
	}

	struct tty *t = &ttys[cur];
	int col = x / 8; /* CHAR_WIDTH */
	int row = y / 16; /* CHAR_HEIGHT */

	if (col < 0 || col >= t->cols || row < 0 || row >= t->rows) {
		return;
	}

	int idx = row * MAX_TTY_COLS + col;
	t->grid[idx].c = c;
	t->grid[idx].fg = fg;
	t->grid[idx].bg = bg;
}

void tty_clear_grid(void)
{
	int cur = atomic_read(&current_tty_id);
	if (cur < 0 || cur >= MAX_TTYS) {
		return;
	}

	struct tty *t = &ttys[cur];
	int total = MAX_TTY_COLS * MAX_TTY_ROWS;

	for (int i = 0; i < total; i++) {
		t->grid[i].c = ' ';
		t->grid[i].fg = 0;
		t->grid[i].bg = 0;
	}
}

/**
 * tty_redraw() - 从网格逐格重绘到 framebuffer。
 *
 * 遍历 rows×cols，每格调用 draw_char() 恢复像素内容。
 */
static void tty_redraw(int id)
{
	if (id < 0 || id >= MAX_TTYS) {
		return;
	}

	struct tty *t = &ttys[id];

	for (int row = 0; row < t->rows; row++) {
		for (int col = 0; col < t->cols; col++) {
			int idx = row * MAX_TTY_COLS + col;
			struct tty_cell *cell = &t->grid[idx];

			/* 未写入过的单元格或黑底空格跳过——fb_clear_screen 已清 */
			if (cell->c == '\0') {
				continue;
			}
			if (cell->c == ' ' && cell->bg == 0) {
				continue;
			}

			draw_char(col * 8, row * 16, cell->c, cell->fg, cell->bg);
		}
	}
}

/**
 * tty_switch() - 切换到指定 TTY，先清屏再重绘目标网格。
 *
 * 历史问题:原来仅赋值 current_tty_id，fb 上残留上一个 TTY 的内容，
 * 切回时前一个 TTY 的字符会"覆盖"在当前 TTY 的输出上。
 *
 * 注意:不走 fb_clear_screen() —— 它会连带调用 tty_clear_grid()
 * 清空目标 TTY 网格，导致 tty_redraw() 无内容可恢复。
 * 这里仅清 fb 像素，保留网格由 tty_record_char 维护。
 *
 * 持 print_lock 序列化 fb 写入，避免与其它 CPU 上的 kprintf() 竞争。
 * 调用方（键盘 ISR）已关中断，arch_spin_lock_irqsave 安全（push 0 → pop 0）。
 */
void tty_switch(int new_id)
{
	if (new_id < 0 || new_id >= MAX_TTYS) {
		return;
	}
	if (new_id == atomic_read(&current_tty_id)) {
		return;
	}

	uint64_t flags;
	arch_spin_lock_irqsave(&print_lock, flags);

	atomic_set(&current_tty_id, new_id);

	/* 直接清 framebuffer 像素，不触发 tty_clear_grid */
	struct limine_framebuffer *fb = fb_get_info();
	if (fb) {
		uint32_t *fb_ptr = (uint32_t *)fb->address;
		uint32_t stride = fb->pitch / 4;
		for (uint32_t y = 0; y < fb->height; y++) {
			for (uint32_t x = 0; x < fb->width; x++) {
				fb_ptr[y * stride + x] = 0;
			}
		}
	}

	tty_redraw(new_id);

	arch_spin_unlock_irqrestore(&print_lock, flags);
}
