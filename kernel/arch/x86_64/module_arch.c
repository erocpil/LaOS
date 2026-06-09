/*
 * module_arch.c - x86_64 I-cache "同步"
 *
 * x86_64 硬件侦听保证数据写入后 I-cache 自动一致。
 * 仅插入编译器屏障防止重排代码修改与执行。
 */
#include "module.h"
#include "arch_dispatch.h"

void arch_module_sync_icache(void *start, size_t size)
{
	(void)start;
	(void)size;
	__asm__ volatile("" ::: "memory");
}

uintptr_t arch_module_alloc_base(void)
{
	return MODULE_REGION_DEFAULT_BASE;
}
