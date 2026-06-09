#ifndef __DEBUG_H__
#define __DEBUG_H__

/*
 * debug.h - 调试函数声明与栈回溯宏
 */

#include <stdint.h>

#include "printf.h"
#include "export.h"
#include "cpu.h"
#include "arch_cpu.h"
#include "config.h"

#define PREEMPT 1
#define SWITCH_MM 1

#define panic(fmt, ...) \
	do { \
		kprintf("[PANIC %s:%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
		asm volatile("cli; hlt"); \
		__asm__ volatile ("int $3"); \
		while (1) { \
			asm volatile("cli; hlt"); \
		} \
	} while (0)

// 让编译器检查 fmt 和后续参数是否匹配
	static inline __attribute__((format(printf, 1, 2)))
void _L_check(const char *fmt, ...)
{
	(void)fmt;
}

/**
 * 获取当前物理 RSP 指针的数值
 */
static inline uint64_t get_rsp(void)
{
	uint64_t rsp;
	__asm__ volatile ("mov %%rsp, %0" : "=r" (rsp));
	return rsp;
}

static inline uint64_t get_gs(void)
{
	uint64_t gs;
	__asm__ volatile ("mov %%gs, %0" : "=r" (gs));
	return gs;
}

/*
   _L_check(fmt "\n", ##__VA_ARGS__);                                \
   */
/*
#define L(fmt, ...) \
do {                                              \
kprintf("[%s %d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__);  \
} while(0)
*/
#if CONFIG_DEBUG
#define L(fmt, ...) \
	do {                                              \
		kprintf("[%s %d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__);  \
	} while(0)
#else
#define L(fmt, ...) \
	do { \
		if (0) kprintf("[%s %d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__);  \
	} while(0)
#endif

#define L1() \
	do { \
		if (1 == cpu_get_ctx()->id) { \
			L("CPU1"); \
		} \
	} while (0)

#define LR() \
	do { \
		kprintf("[%s %d %lu]\n", __func__, __LINE__, rdtsc()); \
	} while (0)

void hcf(void);

static inline int interrupts_enabled(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq\n\t"
			"popq %0"
			: "=r"(flags));
	return (flags >> 9) & 1;   /* IF 是第 9 位 */
}
EXPORT_SYMBOL(interrupts_enabled);

#define in_irq() \
	do { \
		if (interrupts_enabled()) { \
			L("CPU %d IF=1, interrupt on", cpu_get_ctx()->id); \
		} else { \
			L("CPU %d IF=0, interrupt off", cpu_get_ctx()->id); \
		} \
	} while (0)

#endif
