#ifndef __EXPORT_H__
#define __EXPORT_H__

/*
 * export.h - EXPORT_SYMBOL 宏定义
 */

#include <stdint.h>

struct kernel_symbol {
	uint64_t addr;
	const char *name;
};

#define EXPORT_SYMBOL(sym)                                  \
	static const char __ksymtab_str_##sym[]                 \
	__attribute__((used, section("__ksymtab_strings"))) \
	= #sym;                                             \
	static const struct kernel_symbol __ksymtab_##sym       \
	__attribute__((used, section("__ksymtab")))         \
	= { (uint64_t)&sym, __ksymtab_str_##sym }

#endif
