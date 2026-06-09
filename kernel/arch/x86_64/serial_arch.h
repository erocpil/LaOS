/*
 * serial_arch.h — 串口访问 (x86_64)
 *
 * 提供：
 *   arch_serial_putchar(c)  — 往 COM1 (0x3F8) 发送一个字符
 *   arch_serial_read()      — 从键盘端口 (0x60) 读一个字节
 *
 * x86 通过 IO 端口 (inb/outb) 访问。
 * aarch64 通过 MMIO 映射的 PL011 UART 访问。
 */

#ifndef __ARCH_X86_64_SERIAL_ARCH_H__
#define __ARCH_X86_64_SERIAL_ARCH_H__

#include <stdint.h>

/* x86 IO 端口原语 */
static inline uint8_t arch_inb(uint16_t port)
{
	uint8_t ret;
	__asm__ volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
	return ret;
}

static inline void arch_outb(uint16_t port, uint8_t val)
{
	__asm__ volatile("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

/* COM1 串口发送 */
static inline void arch_serial_putchar(char c)
{
	arch_outb(0x3F8, (uint8_t)c);
}

/* COM1 串口轮询读取一个字节。调用前需确认有数据可读(LSR bit 0)。 */
static inline uint8_t arch_serial_read(void)
{
	return arch_inb(0x3F8);
}

/* COM1 初始化：8N1，使能 FIFO + RX 中断 */
static inline void arch_serial_init_com1(void)
{
	/* 禁用中断期间配置 */
	arch_outb(0x3F9, 0x00);  /* IER = 0 */

	/* DLAB=1 → 设波特率除数(115200) */
	arch_outb(0x3FB, 0x80);  /* LCR: DLAB=1 */
	arch_outb(0x3F8, 0x01);  /* DLL = 1  (115200 @ 115200 基准时钟) */
	arch_outb(0x3F9, 0x00);  /* DLM = 0 */

	/* DLAB=0 → 8N1 */
	arch_outb(0x3FB, 0x03);  /* LCR: 8 data bits, 1 stop, no parity */

	/* 使能 FIFO，清空收发 FIFO */
	arch_outb(0x3FA, 0x07);  /* FCR: enable + clear TX + clear RX */

	/* 使能 RX 中断 (bit 0) */
	arch_outb(0x3F9, 0x01);  /* IER: ERBFI (Received Data Available) */
}

/* PS/2 键盘数据端口读取 */
static inline uint8_t arch_keyboard_read(void)
{
	return arch_inb(0x60);
}

#endif
