#ifndef __PMM_H__
#define __PMM_H__

/*
 * Physical Memory Manager (PMM)
 *
 * bitmap-based first-fit 物理页分配器，单页粒度 PAGE_SIZE = 4KB.
 *
 * 调用方契约
 * - pmm_init 在 boot 阶段单线程调用，**不加锁**，传 limine memmap response.
 * - 所有运行期公共函数(pmm_alloc / pmm_free / pmm_alloc_pages /
 *   pmm_free_pages / pmm_print_stats / pmm_dump_bitmap / pmm_print_page)
 *   由 pmm 内部加锁，调用方**不需要持 pinfo.pmm_lock**.
 * - pmm_test_* 是 white-box 自测，故意触碰 bitmap 内部状态，仅开发期跑。
 *
 * 不变量
 * - usable - freed = used(推导，不直接存)
 * - 物理 [0， PMM_LOWMEM_LIMIT=0x100000) 范围(256 页)永远不分配，
 *   由 pmm_idx_safe 守卫。详见 pmm.c 注释和 docs/pmm-review.md.
 * - pmm.c 内部 helper(pmm_account_alloc / pmm_account_free / pmm_alloc_run /
 *   pmm_find_run)的注释会标注"调用方必须持 pinfo.pmm_lock":只是 pmm 内部
 *   实现细节，不影响外部调用方。
 *
 * pmm_total_pages 的物理含义
 * - 是 bitmap **索引上限**，不是物理内存上限。
 * - 当前实现取所有 USABLE 段的最大 end_addr / PAGE_SIZE，
 *   所有 USABLE 页都在 bitmap 覆盖范围内；lost_page 计数器保留作为不变量自检(boot log 里应恒为 0).
 * - pmm_total_pages 和 physical RAM 之间可能还有 reserved 和 ACPI 区，
 *   这些 idx 由 pmm_mark_protected 自检维持 USED 状态。
 *
 * struct pmm_info 字段
 * - page.total:  memmap 报告的所有页(USABLE + Reserved + ACPI + ...)
 * - page.none:   非 USABLE 区域页数
 * - page.usable: 可用页基数(含 self，扣 dont_touch)
 * - page.self:   bitmap 自身占用页数
 * - page.freed:  当前空闲页数(动态维护，应与 bitmap 扫出来的真值一致)
 *
 * 错误处理
 * - pmm_alloc / pmm_alloc_pages 找不到空闲页时返回 NULL，不 panic.
 * - pmm_free 单页 double-free / 越界 -> panic(致命).
 * - pmm_free_pages 多页 double-free -> 单页跳过 + L() 警告(不 panic，
 *   因为多页里混 double-free 的恶劣场景上层应该已经在 panic 路径上了).
 *
 * 设计取舍
 * - 不维护 next_free_page hint:原版三处倒退路径让 hint 在任何 alloc-free-alloc
 *   序列后立即失效，删之后行为完全不变(实测 r1/r2 一致).详见
 *   docs/pmm-review.md 和 pmm.c:354-378 注释。
 * - 不实现 buddy / NUMA / rwlock:LaOS 教学规模 64KB bitmap 全扫几微秒，
 *   YAGNI.详见 docs/pmm-review.md.
 */

#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "lock.h"

#define KIB (1ULL << 10)
#define MIB (1ULL << 20)
#define GIB (1ULL << 30)

/* ---- 架构无关的内存区域描述 ---- */
#define PMM_MEMMAP_USABLE               0
#define PMM_MEMMAP_RESERVED             1
#define PMM_MEMMAP_ACPI_RECLAIMABLE     2
#define PMM_MEMMAP_ACPI_NVS             3
#define PMM_MEMMAP_BAD_MEMORY           4
#define PMM_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define PMM_MEMMAP_EXECUTABLE_AND_MODULES 6
#define PMM_MEMMAP_FRAMEBUFFER          7

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

#define PMM_INFO_DUMP(p) \
	do { \
		__typeof__(p) _p = (p); \
		uint64_t flags = 0; \
		arch_spin_lock_irqsave(&_p->pmm_lock, flags); \
		kprintf("PMM PAGE STATE:\n" \
				"  total  %lu\n" \
				"  none   %lu\n" \
				"  usable %lu\n" \
				"  self   %lu\n" \
				"  used   %lu\n" \
				"  freed  %lu\n", \
				_p->page.total, _p->page.none, _p->page.usable, \
				_p->page.self, _p->page.usable - _p->page.freed, _p->page.freed); \
		arch_spin_unlock_irqrestore(&_p->pmm_lock, flags); \
	} while (0)

/*
 * 关于 PMM_INFO_DUMP 在 pmm_init 中段输出的"中间快照"语义：
 *
 *   pmm_scan 把所有 USABLE 页(含 lowmem 256 页 + bitmap 自身几页)
 *   都计入 pinfo.page.usable / freed，先把基数立住.pmm_init 末尾才做
 *   lowmem / lost(应为 0) / bitmap 三次扣除，得到最终值。
 *
 *   因此 pmm_init 中段(pmm_scan 收尾，release_usable 收尾)的
 *   PMM_INFO_DUMP 输出会显示**未扣除前**的中间值：把它当 scan 阶段
 *   原始统计读，不要拿去和 pmm_dump_bitmap 真实空闲页数对账。最终值
 *   看 pmm_init 末尾的 PMM_INFO_DUMP / pmm_print_stats 输出。
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
