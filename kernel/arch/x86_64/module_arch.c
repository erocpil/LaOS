/*
 * x86_64 模块代码写入后的指令缓存同步接口。
 *
 * x86_64 具有指令与数据缓存一致性。此实现不执行硬件缓存维护，
 * 仅以编译器屏障约束代码写入与后续执行之间的内存访问重排。
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
