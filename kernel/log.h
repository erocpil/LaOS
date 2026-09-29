#ifndef __LOG_H__
#define __LOG_H__

/*
 * log.h - 日志模块标签定义
 */

#include "color.h"

typedef enum {
	LOG_LOADER = 0,
	LOG_BOOT,
	LOG_CPU,
	LOG_PMM,
	LOG_VMM,
	LOG_HEAP,
	LOG_MODULE,
	LOG_PCI,
	LOG_TTY,
	LOG_SYSCALL,
	LOG_NET,
	LOG_MOD_COUNT,
} log_module_e;

typedef struct {
	const char *name;
	int         color;
} log_module_info_t;

// 数组下标必须和 enum 值严格对应，顺序不能错
static const log_module_info_t g_log_modules[LOG_MOD_COUNT] = {
	[LOG_LOADER]  = { "loader",  COLOR_LOADER },
	[LOG_BOOT]    = { "boot",    COLOR_BOOT },
	[LOG_CPU]     = { "cpu",     COLOR_CPU },
	[LOG_PMM]     = { "pmm",     COLOR_PMM },
	[LOG_VMM]     = { "vmm",     COLOR_VMM },
	[LOG_HEAP]    = { "heap",    COLOR_HEAP },
	[LOG_MODULE]  = { "module",  COLOR_MODULE },
	[LOG_PCI]     = { "pci",     COLOR_PCI },
	[LOG_TTY]     = { "tty",     COLOR_TTY },
	[LOG_SYSCALL] = { "syscall", COLOR_SYSCALL },
	[LOG_NET]     = { "net",     COLOR_NET },
};

extern int g_log_tag_width;

int log_get_tag_width(void);

_Static_assert(sizeof(g_log_modules)/sizeof(g_log_modules[0]) == LOG_MOD_COUNT, "g_log_modules size mismatch with LOG_MOD_COUNT");
#define L_TAG(mod, fmt, ...) \
	kprintf_color(g_log_modules[mod].color, \
			"[%*s] " fmt, \
			log_get_tag_width(), g_log_modules[mod].name, ##__VA_ARGS__)

void log_init(void);

#endif
