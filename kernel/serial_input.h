/*
 * serial_input.h — 串口输入字节处理（跨架构共享）
 *
 * ESC 序列 Alt+数字 = TTY 切换，a-z 回显到串口。
 * 由架构中断/轮询代码调用，每字节调用一次。
 */

#ifndef __KERNEL_SERIAL_INPUT_H__
#define __KERNEL_SERIAL_INPUT_H__

#include "tty.h"
#include "monitor.h"

static inline void serial_input_process(uint8_t ch)
{
	static int serial_esc = 0;

	if (serial_esc) {
		serial_esc = 0;
		/* Alt+数字 → TTY N（与 PS/2 键盘一致） */
		if (ch >= '0' && ch <= '9') {
			int tty_id = ch - '0';
			if (tty_id >= 6 && !monitor_ready)
				return;
			tty_switch(tty_id);
		}
		return;
	}
	if (ch == 0x1B) { /* ESC — Alt 前缀 */
		serial_esc = 1;
		return;
	}
	if (ch >= 'a' && ch <= 'z') {
		arch_serial_putchar((char)ch);
	}
}

#endif /* __KERNEL_SERIAL_INPUT_H__ */
