#ifndef __MODULE_ALLOC_H__
#define __MODULE_ALLOC_H__

/*
 * module_alloc.h - 模块加载内存分配
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * module_alloc 为内核模块（.mo / ET_REL）分配加载地址空间。
 *
 * 内核使用 -mcmodel=kernel，代码位于 0xffffffff80000000 附近。模块也采用该代码模型，
 * 因而必须加载在内核文本段 ±2 GiB 范围内，才能满足 R_X86_64_PC32 和 PLT32 重定位的
 * 位移限制。kheap 位于 0xffffa00000000000，与内核区域相距约 105 TiB，不能用于模块加载。
 *
 * 当前实现使用 bump 指针分配，按 4 KiB 页申请物理内存并通过 vmm_map 建立映射；
 * 尚不支持回收或模块卸载。
 *
 * MODULE_REGION_* 常量定义在 arch_dispatch.h 的"内存布局常量"组。
 */

void module_alloc_init(void);
uintptr_t module_region_base(void);
uintptr_t module_region_max(void);
bool module_region_contains(uintptr_t address);

struct module_alloc_checkpoint {
	uintptr_t brk;
	uintptr_t mapped;
};

/*
 * 分配 size 字节的连续虚拟地址空间。
 *
 * 返回地址按 16 字节对齐。物理页按需分配并映射到内核页表；失败时返回 NULL。
 * 当前分配器不回收已分配空间。
 */
void *module_alloc(size_t size);

struct module_alloc_checkpoint module_alloc_checkpoint(void);
void module_alloc_rollback(struct module_alloc_checkpoint checkpoint);

/* 当前 bump 分配器不回收空间；保留此接口供将来的模块卸载实现使用。 */
void module_free(void *ptr);

/* 输出已分配字节数和映射区域上界，供诊断使用。 */
void module_alloc_stats(void);

#endif
