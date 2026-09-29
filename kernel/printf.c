/*
 * printf.c - 格式化输出:vsnprintf,kprintf
 *
 * zero-copy 实现，不支持浮点，支持 %d/%u/%x/%s/%c 及 width/precision 修饰。
 */
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <limine.h>

#include "printf.h"
#include "lock.h"
#include "arch_dispatch.h"
#include "export.h"
#include "debug.h"
#include "font.h"
#include "cpu.h"
#include "tty.h"
#include "color.h"

#define CHAR_WIDTH     8
#define CHAR_HEIGHT    16
#define KPRINTF_BUF_SZ 1024

static inline void putc_safe(char **p, char *end, int *total, char c);

/**
 * serial_putchar() - 往串口发送一个字符
 *
 * 0x3F8 是 COM1 端口
 */
void serial_putchar(char c)
{
	/* 架构相关：x86 通过 IO 端口 outb(0x3F8)，ARM64 通过 MMIO PL011。
	 * 见 arch/x86_64/serial_arch.h : arch_serial_putchar()。 */
	arch_serial_putchar(c);
}
EXPORT_SYMBOL(serial_putchar);

/** kprint_str() - 格式化辅助函数 */
void kprint_str(const char* s)
{
	while (*s) {
		serial_putchar(*s++);
	}
}

/** kprint_int() - 将数字转换为指定进制的字符串并打印 */
void kprint_int(unsigned long n, int base, bool sign)
{
	static const char digits[] = "0123456789abcdef";
	char buf[64];
	int i = 0;

	// 处理负数 (仅限十进制)
	if (sign && (long)n < 0) {
		serial_putchar('-');
		n = -(long)n;
	}

	if (n == 0) {
		serial_putchar('0');
		return;
	}

	while (n > 0) {
		buf[i++] = digits[n % base];
		n /= base;
	}

	// 逆序打印
	while (--i >= 0) {
		serial_putchar(buf[i]);
	}
}

/** putc_safe() - 向缓冲写一个字符，同时无条件递增理论计数器。
 *
 * 截断后停止写入，但 total 继续计数，调用者可借此检测截断。
 */
static inline void putc_safe(char **p, char *end, int *total, char c)
{
	(*total)++;
	if (*p < end) {
		*(*p)++ = c;
	}
}

static void pad_buf(char **p, char *end, int *total, char ch, int n)
{
	while (n-- > 0) {
		putc_safe(p, end, total, ch);
	}
}

/** print_str_w() - 打印字符串(带宽度 / 精度 / 对齐) */
static void print_str_w(char **p, char *end, int *total,
		const char *s, int width, int prec, int left)
{
	if (!s) {
		s = "(null)";
	}

	int len = 0;
	const char *t = s;
	while (*t && (prec < 0 || len < prec)) {
		t++;
		len++;
	}

	int pad_n = (width > len) ? width - len : 0;

	if (!left) {
		pad_buf(p, end, total, ' ', pad_n);
	}
	for (int i = 0; i < len; i++) {
		putc_safe(p, end, total, s[i]);
	}
	if (left) {
		pad_buf(p, end, total, ' ', pad_n);
	}
}

/** print_uint_w() - 打印无符号整数(任意进制，带完整格式化选项) */
static void print_uint_w(char **p, char *end, int *total,
		uint64_t v, int base, int upper,
		int width, int prec, int left, int zero, int prefix)
{
	const char *dlo = "0123456789abcdef";
	const char *dhi = "0123456789ABCDEF";
	const char *digits = upper ? dhi : dlo;

	char tmp[64];
	int n = 0;

	if (v == 0) {
		tmp[n++] = '0';
	} else {
		uint64_t val = v;
		while (val) {
			tmp[n++] = digits[val % base];
			val /= base;
		}
	}

	char pfx[3]  = {0};
	int pfx_len = 0;
	if (prefix && base == 16 && v != 0) {
		pfx[pfx_len++] = '0';
		pfx[pfx_len++] = upper ? 'X' : 'x';
	} else if (prefix && base == 8 && tmp[n - 1] != '0') {
		pfx[pfx_len++] = '0';
	}

	int digit_n = (prec > n) ? prec : n;
	int content = pfx_len + digit_n;
	int pad_n = (width > content) ? width - content : 0;

	if (!left && zero) {
		for (int i = 0; i < pfx_len; i++) {
			putc_safe(p, end, total, pfx[i]);
		}
		pad_buf(p, end, total, '0', pad_n);
	} else if (!left) {
		pad_buf(p, end, total, ' ', pad_n);
		for (int i = 0; i < pfx_len; i++) {
			putc_safe(p, end, total, pfx[i]);
		}
	} else {
		for (int i = 0; i < pfx_len; i++) {
			putc_safe(p, end, total, pfx[i]);
		}
	}

	pad_buf(p, end, total, '0', digit_n - n);
	for (int i = n - 1; i >= 0; i--) {
		putc_safe(p, end, total, tmp[i]);
	}
	if (left) {
		pad_buf(p, end, total, ' ', pad_n);
	}
}

/** print_int_w() - 打印有符号整数 */
static void print_int_w(char **p, char *end, int *total,
		int64_t v, int width, int prec,
		int left, int zero, int force_sign)
{
	char sign = 0;
	uint64_t uv;

	if (v < 0) {
		sign = '-';
		uv = (uint64_t)(-(v + 1)) + 1; /* 安全处理 INT64_MIN */
	} else {
		uv = (uint64_t)v;
		if (force_sign) {
			sign = '+';
		}
	}

	char tmp[22];
	int n = 0;
	if (uv == 0) {
		tmp[n++] = '0';
	} else {
		uint64_t val = uv;
		while (val) {
			tmp[n++] = '0' + (val % 10);
			val /= 10;
		}
	}

	int digit_n = (prec > n) ? prec : n;
	int content = (sign ? 1 : 0) + digit_n;
	int pad_n = (width > content) ? width - content : 0;

	if (!left && zero) {
		if (sign) {
			putc_safe(p, end, total, sign);
		}
		pad_buf(p, end, total, '0', pad_n);
	} else if (!left) {
		pad_buf(p, end, total, ' ', pad_n);
		if (sign) {
			putc_safe(p, end, total, sign);
		}
	} else {
		if (sign) {
			putc_safe(p, end, total, sign);
		}
	}

	pad_buf(p, end, total, '0', digit_n - n);
	for (int i = n - 1; i >= 0; i--) {
		putc_safe(p, end, total, tmp[i]);
	}
	if (left) {
		pad_buf(p, end, total, ' ', pad_n);
	}
}

/** vsnprintf() -  zero-copy */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	if (!buf || size == 0) {
		return 0;
	}

	char *p = buf;
	char *end = buf + size - 1;
	int total = 0;

	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			putc_safe(&p, end, &total, *fmt);
			continue;
		}
		fmt++;

		/* 1. 标志位 */
		int flag_left = 0;
		int flag_zero = 0;
		int flag_plus = 0;
		int flag_space = 0;
		int flag_hash = 0;
		for (;;) {
			if (*fmt == '-') {
				flag_left = 1;
				fmt++;
			} else if (*fmt == '0') {
				flag_zero = 1;
				fmt++;
			} else if (*fmt == '+') {
				flag_plus = 1;
				fmt++;
			} else if (*fmt == ' ') {
				flag_space = 1;
				fmt++;
			} else if (*fmt == '#') {
				flag_hash = 1;
				fmt++;
			} else {
				break;
			}
		}
		if (flag_left) {
			flag_zero = 0;
		}

		/* 2. 宽度 */
		int width = 0;
		if (*fmt == '*') {
			width = va_arg(args, int);
			if (width < 0) {
				flag_left = 1;
				width = -width;
			}
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9') {
				width = width * 10 + (*fmt++ - '0');
			}
		}

		/* 3. 精度 */
		int prec = -1;
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(args, int);
				if (prec < 0) {
					prec = -1;
				}
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9') {
					prec = prec * 10 + (*fmt++ - '0');
				}
			}
		}

		/* 4. 长度修饰符 */
		int lmod = 0;
		switch (*fmt) {
			case 'h':
				fmt++;
				lmod = (*fmt == 'h') ? (fmt++, -2) : -1;
				break;
			case 'l':
				fmt++;
				lmod = (*fmt == 'l') ? (fmt++, 2) : 1;
				break;
			case 'z':
				lmod = 1;
				fmt++;
				break;
			case 't':
				lmod = 1;
				fmt++;
				break;
			default:  break;
		}

		/* 5. 转换说明符 */
		switch (*fmt) {
			case '%':
				putc_safe(&p, end, &total, '%');
				break;
			case 'c': {
						  char c = (char)va_arg(args, int);
						  if (!flag_left) {
							  pad_buf(&p, end, &total, ' ', width - 1);
						  }
						  putc_safe(&p, end, &total, c);
						  if (flag_left) {
							  pad_buf(&p, end, &total, ' ', width - 1);
						  }
						  break;
					  }
			case 's': {
						  const char *s = va_arg(args, const char *);
						  print_str_w(&p, end, &total, s, width, prec, flag_left);
						  break;
					  }
			case 'd':
			case 'i': {
						  int64_t v;
						  if (lmod == 2) {
							  v = va_arg(args, long long);
						  } else if (lmod == 1) {
							  v = va_arg(args, long);
						  } else if (lmod == -1) {
							  v = (short)va_arg(args, int);
						  } else if (lmod == -2) {
							  v = (signed char)va_arg(args, int);
						  } else {
							  v = va_arg(args, int);
						  }
						  print_int_w(&p, end, &total, v, width, prec,
								  flag_left, flag_zero, flag_plus || flag_space);
						  break;
					  }
			case 'u': {
						  uint64_t v;
						  if (lmod == 2) {
							  v = va_arg(args, unsigned long long);
						  } else if (lmod == 1) {
							  v = va_arg(args, unsigned long);
						  } else if (lmod == -1) {
							  v = (unsigned short)va_arg(args, unsigned int);
						  } else if (lmod == -2) {
							  v = (unsigned char)  va_arg(args, unsigned int);
						  } else {
							  v = va_arg(args, unsigned int);
						  }
						  print_uint_w(&p, end, &total, v, 10, 0,
								  width, prec, flag_left, flag_zero, 0);
						  break;
					  }
			case 'x':
			case 'X': {
						  uint64_t v;
						  if (lmod == 2) {
							  v = va_arg(args, unsigned long long);
						  } else if (lmod == 1) {
							  v = va_arg(args, unsigned long);
						  } else if (lmod == -1) {
							  v = (unsigned short)va_arg(args, unsigned int);
						  } else if (lmod == -2) {
							  v = (unsigned char)va_arg(args, unsigned int);
						  } else {
							  v = va_arg(args, unsigned int);
						  }
						  print_uint_w(&p, end, &total, v, 16, *fmt == 'X',
								  width, prec, flag_left, flag_zero, flag_hash);
						  break;
					  }
			case 'o': {
						  uint64_t v;
						  if (lmod == 2) {
							  v = va_arg(args, unsigned long long);
						  } else if (lmod == 1) {
							  v = va_arg(args, unsigned long);
						  } else {
							  v = va_arg(args, unsigned int);
						  }
						  print_uint_w(&p, end, &total, v, 8, 0,
								  width, prec, flag_left, flag_zero, flag_hash);
						  break;
					  }
			case 'p': {
						  uint64_t v = (uint64_t)va_arg(args, void *);
						  putc_safe(&p, end, &total, '0');
						  putc_safe(&p, end, &total, 'x');
						  print_uint_w(&p, end, &total, v, 16, 0,
								  16, 16, 0, 1, 0);
						  break;
					  }
			case 'n':
					  /* 内核中禁用，消耗参数避免 va_list 错位 */
					  (void)va_arg(args, int *);
					  break;
			default:
					  putc_safe(&p, end, &total, '%');
					  putc_safe(&p, end, &total, *fmt);
					  break;
		}
	}

	*p = '\0';

	return total;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	int ret = vsnprintf(buf, size, fmt, args);
	va_end(args);

	return ret;
}

/* 调用者必须保证 buf 足够大 */
int ksprintf(char *buf, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	int ret = vsnprintf(buf, KPRINTF_BUF_SZ, fmt, args);
	va_end(args);

	return ret;
}

// 终端状态全局变量
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;

// 假设这些值从 limine_framebuffer 结构体中获取
struct limine_framebuffer *fb_info;

void *fb_set_info(void *framebuffer)
{
	return fb_info = framebuffer;
}

void *fb_get_info(void)
{
	return (void*)fb_info;
}

void fb_print_string(const char* str, uint32_t fg, uint32_t bg)
{
	if (!fb_info) {
		return;
	}

	uint32_t screen_width = fb_info->width;
	uint32_t screen_height = fb_info->height;

	for (int i = 0; str[i] != '\0'; i++) {
		char c = str[i];

		// 1. 处理换行符
		if (c == '\n') {
			cursor_x = 0;
			cursor_y += CHAR_HEIGHT;
			continue;
		}
		if (c == '\r') {
			cursor_x = 0;
			continue;
		}
		if (c == '\t') {
			uint32_t col = cursor_x / CHAR_WIDTH;
			uint32_t next = (col + 4) & ~3u;
			cursor_x = next * CHAR_WIDTH;
			if (cursor_x >= fb_info->width) {
				cursor_x = 0;
				cursor_y += CHAR_HEIGHT;
			}
			continue;
		}

		// 2. 处理自动换行(如果当前行放不下下一个字符)
		if (cursor_x + CHAR_WIDTH > screen_width) {
			cursor_x = 0;
			cursor_y += CHAR_HEIGHT;
		}

		// 3. 处理垂直滚屏(简单覆盖法：如果超出底部，回到顶部)
		if (cursor_y + CHAR_HEIGHT > screen_height) {
			cursor_y = 0;
			cursor_x = 0;
			// TODO 可以在此处调用一个清屏函数
			// fb_clear_screen(0);
		}

		// 4. 调用上面更新后的 draw_char
		draw_char(cursor_x, cursor_y, c, fg, bg);

		// 5. 移动光标到下一个字符位
		cursor_x += CHAR_WIDTH;
	}
}

void draw_char(int x, int y, char c, uint32_t fg, uint32_t bg)
{
	if (!fb_info) {
		return;
	}

	uint32_t *fb_ptr = (uint32_t*)fb_info->address;
	int stride = fb_info->pitch / 4;

	// 计算该字符在点阵数组中的起始地址
	// 将 c 转换为 unsigned char 是为了防止 ASCII 码大于
	// 127 时变成负数导致索引错误
	const unsigned char *glyph = &fontdata_8x16[(unsigned char)c * 16];

	for (int row = 0; row < 16; row++) {
		// 获取该行 8 个像素的位图
		uint8_t row_data = glyph[row];
		for (int col = 0; col < 8; col++) {
			// 计算当前像素在显存中的线性偏移
			int offset = (y + row) * stride + (x + col);
			// row_data 从最高位(bit 7)开始对应左边的第一个像素
			if ((row_data >> (7 - col)) & 1) {
				fb_ptr[offset] = fg;
			} else {
				fb_ptr[offset] = bg;
			}
		}
	}

	/* fb 写入完成后同步记录到当前 TTY 网格 */
	tty_record_char(x, y, c, fg, bg);
}

static void serial_puts(const char *s)
{
	while (*s) {
		serial_putchar(*s++);
	}
}

extern volatile uint64_t online;
spinlock_t print_lock = SPINLOCK_INIT();
char buf[KPRINTF_BUF_SZ];

/**
 * kprintf() - 格式化字符串
 *
 * 持锁输出到 framebuffer 和串口
 */
void kprintf(const char* fmt, ...)
{
	va_list args;

	uint64_t irq_flags = 0;
	arch_spin_lock_irqsave(&print_lock, irq_flags);

	va_start(args, fmt);
	int written = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	/* 截断检测：在末尾插入标记 */
	if (written >= (int)sizeof(buf)) {
		static const char mark[] = "...[TRUNCATED]\n";
		int mlen = (int)sizeof(mark) - 1;
		int mpos = (int)sizeof(buf) - 1 - mlen;
		if (mpos < 0) {
			mpos = 0;
		}
		__builtin_memcpy(buf + mpos, mark, mlen);
		buf[sizeof(buf) - 1] = '\0';
	}

	/*
	 * TTY 多路复用：只有当前线程的 tty_id 与激活 TTY 一致时才往 fb 写，
	 * 否则其他 TTY 的输出会污染当前显示。详见 tty.c tty_ready()。
	 *
	 * 串口不参与 TTY 复用：它是内核调试通道，相当于 Linux printk 的 console，
	 * 必须始终输出，否则像 monitor.c L("Monitor Started") 这样的早期 L 会在
	 * tty 未对齐窗口里静默丢失，调试盲。
	 *
	 * boot 早期(！SMP)所有路径直接打 fb；SMP 起来后开始 tty 过滤。
	 */
	if (online != g_cpu_count || tty_ready()) {
		fb_print_string(buf, COLOR_WHITE, COLOR_BLACK); /* flush 在内部按需完成 */
	}
	serial_puts(buf);

	arch_spin_unlock_irqrestore(&print_lock, irq_flags);
}
EXPORT_SYMBOL(kprintf);

/**
 * kprintf_color() - 带语义色的 kprintf.
 *
 * 整行一个 fg 色(背景固定 BLACK，避免行间撕裂).串口不上色：
 * serial 是调试通道，混入颜色字节会污染 Linux 端 minicom/screen。
 *
 * boot log 子系统前缀分色用此入口；常规调试用 kprintf。
 */
void kprintf_color(uint32_t fg, const char* fmt, ...)
{
	va_list args;

	uint64_t irq_flags = 0;
	arch_spin_lock_irqsave(&print_lock, irq_flags);

	va_start(args, fmt);
	int written = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (written >= (int)sizeof(buf)) {
		static const char mark[] = "...[TRUNCATED]\n";
		int mlen = (int)sizeof(mark) - 1;
		int mpos = (int)sizeof(buf) - 1 - mlen;
		if (mpos < 0) {
			mpos = 0;
		}
		__builtin_memcpy(buf + mpos, mark, mlen);
		buf[sizeof(buf) - 1] = '\0';
	}

	if (online != g_cpu_count || tty_ready()) {
		fb_print_string(buf, fg, COLOR_BLACK);
	}
	serial_puts(buf);

	arch_spin_unlock_irqrestore(&print_lock, irq_flags);
}
EXPORT_SYMBOL(kprintf_color);

void print_at(int x, int y, uint32_t fg, uint32_t bg, const char *fmt, ...)
{
	uint64_t flags;
	char buf[256];
	va_list args;

	va_start(args, fmt);
	int written = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	arch_spin_lock_irqsave(&print_lock, flags);

	if (written >= (int)sizeof(buf)) {
		serial_puts("[print_at: TRUNCATED]\n");
	}

	int cx = x;
	int cy = y;
	uint32_t y1 = (uint32_t)(y + CHAR_HEIGHT);

	for (int i = 0; buf[i]; i++) {
		if (buf[i] == '\n') {
			cx  = x;
			cy += CHAR_HEIGHT;
			if ((uint32_t)(cy + CHAR_HEIGHT) > y1) {
				y1 = (uint32_t)(cy + CHAR_HEIGHT);
			}
			continue;
		}
		draw_char(cx, cy, buf[i], fg, bg);
		cx += CHAR_WIDTH;
	}

	arch_spin_unlock_irqrestore(&print_lock, flags);
}

void fb_clear_screen(uint32_t color)
{
	uint64_t flags;

	if (!fb_info) {
		return;
	}

	arch_spin_lock_irqsave(&print_lock, flags);

	uint32_t *fb_ptr = (uint32_t*)fb_info->address;
	uint32_t stride = fb_info->pitch / 4;

	// 遍历每一个像素点
	for (uint32_t y = 0; y < fb_info->height; y++) {
		for (uint32_t x = 0; x < fb_info->width; x++) {
			fb_ptr[y * stride + x] = color;
		}
	}

	// 清屏后，光标必须重置回左上角
	cursor_x = 0;
	cursor_y = 0;

	/* fb 清空后同步重置当前 TTY 网格 */
	tty_clear_grid();

	arch_spin_unlock_irqrestore(&print_lock, flags);
}
