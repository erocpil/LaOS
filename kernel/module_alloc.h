#ifndef __MODULE_ALLOC_H__
#define __MODULE_ALLOC_H__

/*
 * module_alloc.h - 模块加载内存分配
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * module_alloc: 给内核模块 (.mo / ET_REL) 加载缓冲区分配虚拟地址。
 *
 * 单独存在的理由：内核以 -mcmodel=kernel 编译，链接到 0xffffffff80000000 起的 -2GB
 * 区；模块切到 -mcmodel=kernel 后，其代码段必须落在距 kernel .text +/-2GB 范围内才能
 * 让 R_X86_64_PC32 / PLT32 重定位正确.kheap 在 0xffffa00000000000，距 kernel 区
 * 约 -105TB，rel32 必然溢出，不能复用。
 *
 * 当前实现：bump 指针，不回收，按 4KB 申请物理页 + vmm_map。模块卸载暂未实现。
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

/**
 * module_alloc() - 分配 size 字节的连续虚拟空间。
 *
 * 按 16 字节对齐返回，物理页按需 pmm_alloc 映射进内核页表。
 * 失败返回 NULL。当前实现是 bump 指针不回收。
 */
void *module_alloc(size_t size);

struct module_alloc_checkpoint module_alloc_checkpoint(void);
void module_alloc_rollback(struct module_alloc_checkpoint checkpoint);

/* 占位：当前 bump 实现下是 no-op。保留接口给未来 module unload。 */
void module_free(void *ptr);

/* 调试：打印当前已分配字节数 / 已映射上界。 */
void module_alloc_stats(void);

#endif
