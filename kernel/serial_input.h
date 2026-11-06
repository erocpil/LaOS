/*
 * serial_input.h — 串口输入字节处理（跨架构共享）
 *
 * Alt+数字序列用于切换 TTY；小写字母回显到串口。架构中断或轮询路径
 * 每接收一个字节调用一次。
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
		/* Alt+数字序列与 PS/2 键盘采用相同的 TTY 编号映射。 */
		if (ch >= '0' && ch <= '9') {
			int tty_id = ch - '0';
			if (tty_id >= 6 && !monitor_ready) {
				return;
			}
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
