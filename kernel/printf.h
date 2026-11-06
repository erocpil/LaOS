#ifndef __PRINTF_H__
#define __PRINTF_H__

/*
 * printf.h - 格式化输出与调试打印
 */

#include <stdint.h>
#include <stdbool.h>
#include "lock.h"

/* framebuffer/TTY 输出锁，由 kprintf 与直接操作 framebuffer 的
 * tty_switch()、fb_clear_screen() 和 print_at() 共用。 */
extern spinlock_t print_lock;

void kprintf(const char *fmt, ...);
void kprintf_color(uint32_t fg, const char *fmt, ...);
int ksprintf(char *buf, const char *fmt, ...);
void draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void print_at(int x, int y, uint32_t fg, uint32_t bg, const char *fmt, ...);
void kprint_int(unsigned long n, int base, bool sign);
void *fb_set_info(void *framebuffer);
void *fb_get_info(void);
void fb_clear_screen(uint32_t color);
void serial_putchar(char c);

#endif
