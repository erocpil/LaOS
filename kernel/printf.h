#ifndef __PRINTF_H__
#define __PRINTF_H__

/*
 * printf.h - 格式化输出与调试打印
 */

#include <stdint.h>
#include <stdbool.h>
#include "lock.h"

/* Framebuffer/TTY serialisation lock — shared between kprintf and
 * direct-fb callers (tty_switch, fb_clear_screen, print_at). */
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
