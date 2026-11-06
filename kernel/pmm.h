#ifndef __PMM_H__
#define __PMM_H__

/*
 * 物理页分配器（PMM）
 *
 * PMM 使用位图管理物理页，按 PAGE_SIZE 为单位进行首次适配分配。
 *
 * 初始化接口在启动阶段调用。运行期分配、释放和统计接口自行保护共享
 * 状态，调用方无需持有 pinfo.pmm_lock。自测接口用于验证分配器内部状态。
 *
 * 分配器不变量：page.usable - page.freed 等于已分配的可用页数；低于
 * 1 MiB 的物理页不参与分配。内部辅助函数的锁约定不构成公共接口契约。
 *
 * pmm_total_pages 是位图索引上限，按所有可用内存区域的最高结束地址计算，
 * 不等同于物理内存容量。位图初始将页面标记为已占用，初始化时仅释放
 * 内存映射标记为可用的页面；其他区域保持占用状态。
 *
 * struct pmm_info 中的 page 字段记录内存映射总页数、不可用页数、可用页数、
 * 位图占用页数及当前空闲页数。
 *
 * 分配失败时分配接口返回 NULL。释放接口对无效地址或重复释放的处理，
 * 以各接口实现为准。
 */

#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "lock.h"

#define KIB (1ULL << 10)
#define MIB (1ULL << 20)
#define GIB (1ULL << 30)

/* ---- 架构无关的内存区域描述 ---- */
#define PMM_MEMMAP_USABLE		  0
#define PMM_MEMMAP_RESERVED		  1
#define PMM_MEMMAP_ACPI_RECLAIMABLE	  2
#define PMM_MEMMAP_ACPI_NVS		  3
#define PMM_MEMMAP_BAD_MEMORY		  4
#define PMM_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define PMM_MEMMAP_EXECUTABLE_AND_MODULES 6
#define PMM_MEMMAP_FRAMEBUFFER		  7

struct pmm_memmap_entry {
	uint64_t base;
	uint64_t length;
	int type;
};

struct pmm_memmap {
	uint64_t count;
	struct pmm_memmap_entry *entries; /* 指向条目数组的指针 */
};

extern struct pmm_info pinfo;

struct pmm_info {
	struct {
		uint64_t total;
		uint64_t none;
		uint64_t self;
		uint64_t usable;
		uint64_t freed;
	} page;
	spinlock_t pmm_lock;
};

#define PMM_INFO_DUMP(p)                                                                           \
	do {                                                                                       \
		__typeof__(p) _p = (p);                                                            \
		uint64_t flags = 0;                                                                \
		arch_spin_lock_irqsave(&_p->pmm_lock, flags);                                      \
		kprintf("PMM PAGE STATE:\n"                                                        \
			"  total  %lu\n"                                                           \
			"  none   %lu\n"                                                           \
			"  usable %lu\n"                                                           \
			"  self   %lu\n"                                                           \
			"  used   %lu\n"                                                           \
			"  freed  %lu\n",                                                          \
			_p->page.total, _p->page.none, _p->page.usable, _p->page.self,             \
			_p->page.usable - _p->page.freed, _p->page.freed);                         \
		arch_spin_unlock_irqrestore(&_p->pmm_lock, flags);                                 \
	} while (0)

/*
 * PMM_INFO_DUMP 在初始化中途输出的是阶段性统计：
 *
 *   pmm_scan 先统计所有 USABLE 页，其中包含低端保留页和位图占用页。
 *   初始化后续阶段再扣除低端保留页、越界页及位图占用页，得到最终计数。
 *
 *   因此初始化中途的输出反映当时已完成的统计步骤，不代表最终空闲页数。
 *   最终计数以初始化完成后的 PMM_INFO_DUMP 或 pmm_print_stats() 为准。
 */

void pmm_init_from_memmap(struct pmm_memmap *mmap);
void *pmm_alloc(void);
void pmm_free(void *addr);
void *pmm_alloc_pages(uint64_t count);
void pmm_free_pages(void *ptr, uint64_t count);
char *mmstr(int type);
void pmm_print_page(void *phys);
void pmm_print_stats(void);
void pmm_dump_bitmap(void);

#if CONFIG_PMM_SELFTEST
void pmm_test_recycling(void);
void pmm_test_lowmem_guard(void);
#endif

#endif
