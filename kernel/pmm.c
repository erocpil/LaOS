/*
 * pmm.c - 物理页分配器
 *
 * Bitmap-based first-fit 分配器，单页粒度 PAGE_SIZE = 4KB.
 * 详见 pmm.h 契约与 docs/pmm-review.md.
 */
#include "pmm.h"
#include "cpu.h"
#include "lock.h"
#include "debug.h"
#include "config.h"
#include "define.h"
#include "string.h"
#include "printf.h"
#include "hhdm.h"
#include "log.h"

uint8_t *pmm_bitmap;
uint64_t pmm_total_pages = 0;
struct pmm_info pinfo = { 0 };

// 简单的位操作宏
#define BITMAP_SET(idx)   (pmm_bitmap[(idx) / 8] |=  (1 << ((idx) % 8)))
#define BITMAP_CLEAR(idx) (pmm_bitmap[(idx) / 8] &= ~(1 << ((idx) % 8)))
#define BITMAP_TEST(idx)  (pmm_bitmap[(idx) / 8] &   (1 << ((idx) % 8)))

/*
 * 1MB 以下不可分配，定义见 pmm_init 里的"硬件雷区"长注释。
 *
 * 显式化守卫。原来只靠 pmm_init 阶段把 0-255 * 这 256 页 mark_used
 * 来挡住 alloc:能挡住，但隐式：任何让 hint 倒退 * 到 < 256 的路径配合
 * bitmap 状态错乱(比如未来某个 bug 错误 free 了 * 一页低端内存)
 * 就会把硬件雷区分出去.alloc 路径自己显式判一下，
 * 即使 bitmap 错位也守住"alloc 出的物理地址 >= 1MB"这个不变量。
 */
#define PMM_LOWMEM_LIMIT 0x100000UL
#define PMM_LOWMEM_PAGES (PMM_LOWMEM_LIMIT / PAGE_SIZE) // 256

static inline bool pmm_idx_safe(uint64_t idx)
{
	return idx >= PMM_LOWMEM_PAGES && idx < pmm_total_pages;
}

/**
 * pmm_account_alloc()
 *
 * 计数 helper: 调用方必须持 pinfo.pmm_lock.
 * 内部不再加锁，避免与持锁的 alloc/free 路径形成嵌套。
 *
 * 设计取舍：原来 pmm_set_info(count， flag) 用一个 int 翻方向，
 * 调用点 (1， 0) / (1， 1) 完全靠死记硬背。
 * 拆成两个命名 helper 后调用点意图自明。
 */
static inline void pmm_account_alloc(uint64_t count)
{
	pinfo.page.freed -= count;
}

static inline void pmm_account_free(uint64_t count)
{
	pinfo.page.freed += count;
}

static void pmm_mark_used(uint64_t page_idx)
{
	if (page_idx >= pmm_total_pages) {
		panic("pmm_mark_used: invalid page idx %lu (total %lu)",
				page_idx, pmm_total_pages);
	}
	BITMAP_SET(page_idx);
}

static void pmm_mark_free(uint64_t page_idx)
{
	if (page_idx >= pmm_total_pages) {
		panic("pmm_mark_free: invalid page idx %lu (total %lu)",
				page_idx, pmm_total_pages);
	}
	BITMAP_CLEAR(page_idx);
}

static void pmm_scan(struct pmm_memmap *mmap)
{
	memset(&pinfo, 0, sizeof(pinfo));
	spin_lock_init(&pinfo.pmm_lock);

	L("Memory map received. Entry count: %lu", mmap->count);

	L("%-6s  %-18s  %-18s  %-18s  %-25s  %s",
			"Region", "Base", "Length", "End", "Type", "Pages");
	L("%-6s  %-18s  %-18s  %-18s  %-25s  %s",
			"------", "------------------", "------------------",
			"------------------", "-------------------------", "--------");
	for (uint64_t i = 0; i < mmap->count; i++) {
		struct pmm_memmap_entry *entry = &mmap->entries[i];
		uint64_t end = entry->base + entry->length;
		uint64_t pages = entry->length / PAGE_SIZE;

		L("%6lu  0x%016lx  0x%016lx  0x%016lx  %-25s  %8lu",
				i, entry->base, entry->length, end, mmstr(entry->type), pages);
		pinfo.page.total += pages;
		if (PMM_MEMMAP_USABLE != entry->type) {
			pinfo.page.none += pages;
		} else {
			pinfo.page.usable += pages;
		}
	}

	pinfo.page.freed = pinfo.page.usable;
}

/**
 * pmm_pick_max_entry() - 扫一遍 memmap 找最大 USABLE 段；计算所有 USABLE 段的最大 end_addr.
 *
 * bitmap 会落在最大段(max_entry)的开头：选最大段的好处是 bitmap 占走
 * 的几页相对段总长可以忽略。但 bitmap 的覆盖范围(pmm_total_pages)取
 * 所有 USABLE 段的最大 end_addr，而不是 max_entry 自身的右边界：避免
 * max_entry 右边的小 USABLE 段无法被 mark_free.
 *
 * 输出参数：
 *   *out_max_end_addr:所有 USABLE 段的最大 (base + length)，pmm_total_pages 由此推导。
 *   实测 max_entry 在中段，其右还有 7 页 USABLE，转正后这 7 页归入 bitmap 覆盖范围。
 *
 * 历史 bug:原来用 `max_entry_count` 当循环计数器，最后取
 * "最后一个 USABLE entry"：靠 limine 通常把最大段排在末尾来侥幸正确。
 *
 * 历史 bug：原来再扫一遍找 length >= bitmap_size 的最小段(best-fit 选错方向)，
 * bitmap 会把那个小段几乎用光(但这样好像也没有问题)。
 *
 * 找不到 USABLE 段 -> panic(系统无法启动)。
 */
static struct pmm_memmap_entry *pmm_pick_max_entry(
		struct pmm_memmap *mmap, uint64_t *out_max_end_addr)
{
	struct pmm_memmap_entry *max_entry = NULL;
	uint64_t max_length = 0;
	uint64_t max_end_addr = 0;

	for (uint64_t i = 0; i < mmap->count; i++) {
		struct pmm_memmap_entry *entry = &mmap->entries[i];
		if (entry->type != PMM_MEMMAP_USABLE) {
			continue;
		}
		if (entry->length > max_length) {
			max_length = entry->length;
			max_entry = entry;
		}
		uint64_t end = entry->base + entry->length;
		if (end > max_end_addr) {
			max_end_addr = end;
		}
	}
	if (!max_entry) {
		panic("no usable entry\n");
	}
	L("Largest Region: Base %p Length %p End %p Type %s size %dpages",
			max_entry->base, max_entry->length,
			(uint64_t)max_entry->base + (uint64_t)max_entry->length,
			mmstr(max_entry->type), max_entry->length / PAGE_SIZE);
	L("Max USABLE end_addr across all entries: %p", max_end_addr);

	*out_max_end_addr = max_end_addr;

	return max_entry;
}

/**
 * pmm_install_bitmap() - 在 max_entry 起始处落 bitmap，初始化为全 1(USED).
 *
 * pmm_total_pages 取 max_end_addr(所有 USABLE 段的最大 end_addr 向上取整)
 * bitmap 覆盖到这里，max_entry 右边的小 USABLE 段也能被 mark_free。
 * bitmap 索引上限和物理内存上限仍然不是同义词:max_end_addr 和 physical RAM
 * 之间可能还有 reserved / ACPI 区，这些 idx 由 mark_protected 自检保持 USED。
 *
 * 历史：原本 pmm_total_pages 只算到 max_entry 右边界，
 * max_entry 右边的 USABLE 段(dev03 实测 7 页 = 28 KB)会被 lost_page 记账丢弃。
 * 转正后 lost_page 应恒 0(保留计数器作为不变量自检)。
 *
 * 返回 bitmap 占用的页数(写入 pinfo.page.self 的来源)。
 */
static uint64_t pmm_install_bitmap(struct pmm_memmap_entry *max_entry,
		uint64_t max_end_addr)
{
	pmm_total_pages = (max_end_addr + (PAGE_SIZE - 1)) / PAGE_SIZE;
	uint64_t bitmap_size = (pmm_total_pages + 7) / 8;
	uint64_t bitmap_pages = (bitmap_size + (PAGE_SIZE - 1)) / PAGE_SIZE;
	L("total pages %lu bitmap size %lu bitmap pages %lu",
			pmm_total_pages, bitmap_size, bitmap_pages);

	/*
	 * bitmap 直接落在 max_entry 开头。max_entry 已经是最大 USABLE 段，length
	 * 必然 >= bitmap_size：仍然 assert 防 memmap 全是小碎片的极端场景。
	 */
	if (max_entry->length < bitmap_size) {
		panic("PMM FATAL: largest usable region (%lu B) too small for bitmap (%lu B)",
				max_entry->length, bitmap_size);
	}

	pmm_bitmap = (uint8_t*)phys_to_virt(max_entry->base);
	memset(pmm_bitmap, 0xff, bitmap_size);
	L("bit map location %p length %lu proper entry base %p length %lu",
			pmm_bitmap, bitmap_size, max_entry->base, max_entry->length);

	return bitmap_pages;
}

/**
 * pmm_release_usable() - 扫 memmap，把所有 USABLE 页 mark_free 到 bitmap，
 * 过滤 lowmem 雷区与 max_entry 右边越界页。
 *
 * 输出参数：
 *   *out_dont_touch_page:被 PMM_LOWMEM_LIMIT 守卫挡住的页数
 *   *out_lost_page :应恒为 0(保留作为不变量自检)
 *
 * USABLE 段列表 + 总页数 + non-USABLE 页数已经由 pmm_scan 阶段打过
 * 一遍(含字节单位换算)，本函数只做 mark_free 与 lowmem/lost_page 记账，
 * 不再重复打字节统计：避免和 pmm_scan 出现两份口径。
 */
static void pmm_release_usable(struct pmm_memmap *mmap,
		uint64_t *out_dont_touch_page, uint64_t *out_lost_page)
{
	uint64_t dont_touch_page = 0;
	uint64_t lost_page = 0;

	for (uint64_t i = 0; i < mmap->count; i++) {
		struct pmm_memmap_entry *entry = &mmap->entries[i];
		if (entry->type != PMM_MEMMAP_USABLE) {
			continue;
		}

		for (uint64_t j = 0; j < entry->length; j += PAGE_SIZE) {
			uint64_t page_phys = entry->base + j;
			uint64_t idx = page_phys / PAGE_SIZE;
			/*
			 * pmm_total_pages 现在取所有 USABLE 段的最大 end_addr，
			 * 任意 USABLE 段内的 idx 都应该 < pmm_total_pages.这里保留计数器
			 * 作为不变量自检:boot log 里 lost_page 应恒为 0。
			 */
			if (idx >= pmm_total_pages) {
				lost_page++;
				continue;
			}
			/*
			 * 跳过 1MB 以下的所有内存：硬件和 BIOS 雷区。
			 *
			 * 在传统 x86 架构中，0x00000-0x100000 这 1MB 是常规内存
			 * (Conventional Memory).Limine 标记为 Usable，但实际千疮百孔：
			 *   0x00000 - 0x004FF: 中断向量表 (IVT) 和 BIOS 数据区 (BDA)
			 *   0x00500 - 0x07BFF: 自由使用的低端内存(通常安全)
			 *   0x07C00 - 0x07FFF: 引导扇区加载位置
			 *   0x80000 - 0x9FFFF: 最危险区域，某些虚拟机显存映射 / SCSI
			 *                      控制器 BIOS / EBDA 可能动态覆盖此地址
			 */
			if (page_phys < PMM_LOWMEM_LIMIT) {
				dont_touch_page++;
				continue;
			}

			pmm_mark_free(idx);
		}
	}

	L("release_usable: dont_touch_page %lu lost_page %lu",
			dont_touch_page, lost_page);

	*out_dont_touch_page = dont_touch_page;
	*out_lost_page = lost_page;
}

/**
 * pmm_mark_protected() - 不变量自检：所有非 USABLE 区域应该已经是 USED。
 *
 * install_bitmap 阶段 memset(0xff) 让 bitmap 初值全 USED，release_usable
 * 只对 USABLE entry 内的 idx mark_free:所以非 USABLE entry 的 idx
 * 必然还是 USED。这里走一遍非 USABLE entry，撞到 free idx 直接 panic：
 * 要么 release_usable 越界，要么 bitmap 被踩了。
 *
 * 跳过 max_end_addr 之外的 entry(bitmap 不覆盖那里)；entry 跨过
 * pmm_total_pages 上界的尾部 break。
 *
 * 历史 bug：旧条件 entry->base > max_entry->base 字段写错：
 * 本意"跳过 max_entry 右边"应该用 >= base + length。
 * 后来进一步改为 >= max_end_addr: bitmap 现在覆盖到 max_end_addr,
 * max_entry 右边的非 USABLE entry 也应该被纳入自检。
 */
static void pmm_mark_protected(struct pmm_memmap *mmap,
		uint64_t max_end_addr)
{
	for (uint64_t i = 0; i < mmap->count; i++) {
		struct pmm_memmap_entry *entry = &mmap->entries[i];
		if (entry->base >= max_end_addr) {
			continue;
		}
		if (entry->type == PMM_MEMMAP_USABLE) {
			continue;
		}

		uint64_t start_page = entry->base / PAGE_SIZE;
		for (uint64_t j = 0; j < entry->length; j += PAGE_SIZE) {
			uint64_t idx = start_page + j / PAGE_SIZE;
			if (idx >= pmm_total_pages) {
				break;
			}
			if (!BITMAP_TEST(idx)) {
				panic("pmm_mark_protected: idx %lu (type %s) is FREE, expected USED",
						idx, mmstr(entry->type));
			}
		}
	}
}

void pmm_init_from_memmap(struct pmm_memmap *mmap)
{
	/* 1. 扫 memmap，统计 USABLE / 非 USABLE 页数(pinfo 基数立住) */
	pmm_scan(mmap);

	/* 2. 选最大 USABLE 段，落 bitmap，初始化为全 1(USED) */
	uint64_t max_end_addr = 0;
	struct pmm_memmap_entry *max_entry = pmm_pick_max_entry(mmap, &max_end_addr);

	uint64_t bitmap_pages = pmm_install_bitmap(max_entry, max_end_addr);

	/* 3. 释放 USABLE 页(过滤 lowmem + max_entry 右边越界页) */
	uint64_t dont_touch_page = 0;
	uint64_t lost_page = 0;
	pmm_release_usable(mmap, &dont_touch_page, &lost_page);

	uint64_t bitmap_page_start = max_entry->base / PAGE_SIZE;
	pinfo.page.self = bitmap_pages;
	pinfo.page.usable -= dont_touch_page;
	pinfo.page.freed -= dont_touch_page;
	pinfo.page.usable -= lost_page;
	pinfo.page.freed -= lost_page;
	pinfo.page.freed -= bitmap_pages;
	for (uint64_t j = 0; j < bitmap_pages; j++) {
		if (!BITMAP_TEST(bitmap_page_start + j)) {
			L("set bitmap for bitmap page");
			pmm_mark_used(bitmap_page_start + j);
		}
	}

	/* 5. 不变量自检：所有非 USABLE 区域应已是 USED */
	pmm_mark_protected(mmap, max_end_addr);

	arch_cpu_early_init();

#if CONFIG_PMM_SELFTEST
	pmm_test_recycling();
	pmm_test_lowmem_guard();
#endif

	L_TAG(LOG_PMM, "PMM initialized.\n");
}

/**
 * pmm_find_run() - 在 [lo， hi) 区间找 count 个连续可分页，找到返回起始 idx，
 * 否则返回 (uint64_t)-1.调用方必须持 pinfo.pmm_lock。
 *
 * "可分"= bitmap free 且 pmm_idx_safe(idx >= 256, < pmm_total_pages).
 * 不安全 idx 会打断 run 以保物理连续性。
 *
 * 取代了原来 pmm_alloc_pages 里的 ({...}) 块语句宏。
 * helper 化后 pmm_alloc 单页路径也走这条逻辑，单页/多页语义统一。
 */
static uint64_t pmm_find_run(uint64_t lo, uint64_t hi, uint64_t count)
{
	uint64_t s = 0;
	uint64_t n = 0;

	for (uint64_t i = lo; i < hi; i++) {
		if (!BITMAP_TEST(i) && pmm_idx_safe(i)) {
			if (n == 0) {
				s = i;
			}
			if (++n >= count) {
				return s;
			}
		} else {
			n = 0;
		}
	}

	return (uint64_t)-1;
}

/**
 * pmm_alloc_run() - alloc 路径的统一实现。
 *
 * 纯线性 first-fit: 从 idx 0 起扫到 pmm_total_pages，找第一个长度
 * 满足的连续 free run。找到则 mark used，记账，返回物理地址；否则返回 NULL.
 *
 * 调用方必须持 pinfo.pmm_lock。
 *
 * 历史：曾经维护 next_free_page hint 让 alloc 不从 0 开始扫，但实测三处倒退
 * 路径(alloc 第二趟 + free 单页 + free 多页)让 hint 在任何 alloc-free-alloc
 * 序列后立即失效：保它的代价(5 处更新点 + 双趟搜索复杂度)拿不回性能。
 * LaOS 教学规模 64KB bitmap 全扫几微秒，删 hint 让代码逻辑显著清晰。
 */
static void *pmm_alloc_run(uint64_t count)
{
	uint64_t start = pmm_find_run(0, pmm_total_pages, count);
	if (start == (uint64_t)-1) {
		return NULL;
	}

	for (uint64_t i = 0; i < count; i++) {
		BITMAP_SET(start + i);
	}
	pmm_account_alloc(count);

	return (void*)(start * PAGE_SIZE);
}

void *pmm_alloc(void)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&pinfo.pmm_lock, flags);
	void *ret = pmm_alloc_run(1);
	arch_spin_unlock_irqrestore(&pinfo.pmm_lock, flags);

	return ret;
}
EXPORT_SYMBOL(pmm_alloc);

void pmm_free(void *addr)
{
	if (addr == NULL) {
		return;
	}

	/* 对齐检查：未对齐地址会导致 page_idx 偏到错误的 bitmap 位，
	 * 静默释放掉保留区或被其他用途使用的页面。 */
	if ((uint64_t)addr & (PAGE_SIZE - 1)) {
		panic("pmm_free: unaligned addr %p", addr);
	}

	uint64_t page_idx = (uint64_t)addr / PAGE_SIZE;
	if (page_idx >= pmm_total_pages) {
		panic("pmm_free: invalid addr %p (idx=%lu, total=%lu)",
				addr, page_idx, pmm_total_pages);
	}

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&pinfo.pmm_lock, flags);

	if (!BITMAP_TEST(page_idx)) {
		arch_spin_unlock_irqrestore(&pinfo.pmm_lock, flags);
		panic("pmm_free: double free at %p (idx=%lu)", addr, page_idx);
	}

	BITMAP_CLEAR(page_idx);
	pmm_account_free(1);

	arch_spin_unlock_irqrestore(&pinfo.pmm_lock, flags);
}
EXPORT_SYMBOL(pmm_free);

void *pmm_alloc_pages(uint64_t count)
{
	if (count == 0) {
		return NULL;
	}

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&pinfo.pmm_lock, flags);

	void *ret = pmm_alloc_run(count);

	arch_spin_unlock_irqrestore(&pinfo.pmm_lock, flags);

	if (!ret) {
		L("PMM cannot alloc %lu contiguous pages", count);
	}

	return ret;
}
EXPORT_SYMBOL(pmm_alloc_pages);

/**
 * pmm_free_pages() - 释放从 ptr 开始的 count 个连续页。
 *
 * 单页释放可继续用原有的 pmm_free()。
 */
void pmm_free_pages(void *ptr, uint64_t count)
{
	if (!ptr || count == 0) {
		return;
	}

	uint64_t start = (uint64_t)ptr / PAGE_SIZE;

	/* 整体范围越界检查，在加锁前做，避免持锁 panic */
	if (start >= pmm_total_pages) {
		panic("pmm_free_pages: invalid addr %p (idx=%lu, total=%lu)",
				ptr, start, pmm_total_pages);
	}
	if (start + count > pmm_total_pages) {
		panic("pmm_free_pages: range [%lu, %lu) exceeds total %lu",
				start, start + count, pmm_total_pages);
	}

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&pinfo.pmm_lock, flags);

	for (uint64_t i = 0; i < count; i++) {
		uint64_t idx = start + i;
		if (!BITMAP_TEST(idx)) {
			arch_spin_unlock_irqrestore(&pinfo.pmm_lock, flags);
			panic("pmm_free_pages: double free at %p (idx=%lu)",
					(void*)(idx * PAGE_SIZE), idx);
		}
		BITMAP_CLEAR(idx);
	}

	pmm_account_free(count);

	arch_spin_unlock_irqrestore(&pinfo.pmm_lock, flags);
}

#if CONFIG_PMM_SELFTEST
/**
 * pmm_test_lowmem_guard() - 验证 1MB 守卫。
 *
 * 思路：
 *   1. 人为 BITMAP_CLEAR 一页低端内存(idx=100， 物理 0x64000),
 *      模拟"bitmap 状态错乱让低端内存看起来 free"的极端场景。
 *   2. 调 pmm_alloc / pmm_alloc_pages，验证返回值 >= PMM_LOWMEM_LIMIT.
 *      纯线性 first-fit 默认从 idx=0 起扫，必先撞到 idx=100，因此
 *      只有 pmm_idx_safe 守卫起作用才能跳过它。
 *   3. 收口：把 idx=100 标回 USED，恢复 bitmap 一致状态。
 *
 * 这个测试故意触碰 bitmap 内部状态，是 white-box 测试，只在开发期跑。
 */
void pmm_test_lowmem_guard(void)
{
	kprintf("--- PMM Lowmem Guard Test Start ---\n");

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&pinfo.pmm_lock, flags);

	BITMAP_CLEAR(100);                  // 人为放出 0x64000 这页

	arch_spin_unlock_irqrestore(&pinfo.pmm_lock, flags);

	// Case 1: pmm_alloc 必须跳过 idx=100
	void *p = pmm_alloc();
	if (p == NULL) {
		kprintf("  FAIL: pmm_alloc returned NULL\n");
	} else if ((uint64_t)p < PMM_LOWMEM_LIMIT) {
		kprintf("  FAIL: pmm_alloc returned lowmem %p (< 1MB)\n", p);
		panic("lowmem guard broken");
	} else {
		kprintf("  PASS: pmm_alloc returned %p (>= 1MB)\n", p);
		pmm_free(p);
	}

	// Case 2: pmm_alloc_pages 起点必须 >= 1MB
	void *pp = pmm_alloc_pages(4);
	if (pp == NULL) {
		kprintf("  FAIL: pmm_alloc_pages(4) returned NULL\n");
	} else if ((uint64_t)pp < PMM_LOWMEM_LIMIT) {
		kprintf("  FAIL: pmm_alloc_pages returned lowmem %p (< 1MB)\n", pp);
		panic("lowmem guard broken");
	} else {
		kprintf("  PASS: pmm_alloc_pages(4) returned %p (>= 1MB)\n", pp);
		pmm_free_pages(pp, 4);
	}

	// 收口：把 idx=100 标回 USED，恢复 bitmap 一致状态
	arch_spin_lock_irqsave(&pinfo.pmm_lock, flags);

	BITMAP_SET(100);

	arch_spin_unlock_irqrestore(&pinfo.pmm_lock, flags);

	kprintf("--- PMM Lowmem Guard Test Complete ---\n");
}

void pmm_test_recycling(void)
{
	kprintf("--- PMM Recycling Test Start ---\n");

	// 1. 初始状态打印
	kprintf("Step 1: Initial state\n");
	pmm_print_stats(); // 假设已有这个函数打印 Free Pages

	// 2. 连续申请一组页面
	kprintf("Step 2: Allocating 5 pages...\n");
	void *p1 = pmm_alloc();
	void *p2 = pmm_alloc();
	void *p3 = pmm_alloc();
	void *p4 = pmm_alloc();
	void *p5 = pmm_alloc();

	kprintf("  Allocated: %p, %p, %p, %p, %p\n", p1, p2, p3, p4, p5);
	pmm_print_stats();

	// 3. 释放中间的页面 (产生碎片)
	kprintf("Step 3: Freeing middle pages (p2, p4)...\n");
	pmm_free(p2);
	pmm_free(p4);

	kprintf("  Freed: %p and %p\n", p2, p4);
	pmm_print_stats();

	// 4. 重新申请，观察是否复用了刚才释放的地址
	kprintf("Step 4: Re-allocating 2 pages to check reuse...\n");
	void *r1 = pmm_alloc();
	void *r2 = pmm_alloc();

	kprintf("  New Allocations: %p, %p\n", r1, r2);

	// 逻辑校验
	if (r1 == p2 || r1 == p4) {
		kprintf("  SUCCESS: Re-allocated page r1 (%p) reused a freed slot.\n", r1);
	} else {
		kprintf("  INFO: r1 (%p) did not reuse freed slots (this depends on your allocation policy).\n", r1);
	}

	if (r2 == p2 || r2 == p4) {
		kprintf("  SUCCESS: Re-allocated page r2 (%p) reused a freed slot.\n", r2);
	}

	// 5. 全部释放，检查最终统计是否归位
	kprintf("Step 5: Freeing all test pages...\n");
	pmm_free(p1);
	pmm_free(p3);
	pmm_free(p5);
	pmm_free(r1);
	pmm_free(r2);

	pmm_print_stats();
	kprintf("--- PMM Recycling Test Complete ---\n");
}
#endif /* CONFIG_PMM_SELFTEST */

void pmm_print_page(void *phys)
{
	kprintf("phys %p Page #%d\n", phys, (uint64_t)phys / PAGE_SIZE);
}

/**
 * pmm_dump_bitmap() - 打印 PMM 内存位图的分布情况
 *
 * 显示每一段连续状态(空闲/占用)的起止页码及对应的物理地址
 *
 * 注意：本函数不持 pinfo.pmm_lock 全程扫 bitmap，输出可能在 SMP 并发
 * alloc/free 下撕裂(状态边界算错).调用方需自行保证 quiesce:单核
 * 调试 / 主核启动早期 / 显式停其它核后再调。日常诊断够用。
 *
 * 若未来需要 SMP-safe 版本：持锁阶段把边界数组先收集到临时 buffer，
 * 解锁后再 L() 打日志，避免持锁打 console.
 */
void pmm_dump_bitmap(void)
{
	if (pmm_total_pages == 0) {
		kprintf("[PMM] No pages tracked.\n");
		return;
	}

	L("--- PMM Bitmap Dump (Total: %u pages) ---", (uint32_t)pmm_total_pages);
	L("%-6s | %-10s | %-10s | %7s | %s",
			"Status", "Start Page", "End Page", "Count", "Phys Address Range");
	L("-------+------------+------------+---------+----------------------------------------");

	int last_status = BITMAP_TEST(0) ? 1 : 0;
	uint64_t start_page  = 0;

	for (uint64_t i = 1; i <= pmm_total_pages; i++) {
		int current_status;
		if (i < pmm_total_pages) {
			current_status = BITMAP_TEST(i) ? 1 : 0;
		} else {
			current_status = !last_status; /* 强制闭合最后一段 */
		}

		if (current_status != last_status) {
			uint64_t end_page = i - 1;
			uint64_t count = end_page - start_page + 1;
			uint64_t start_addr = start_page * PAGE_SIZE;
			uint64_t end_addr = end_page * PAGE_SIZE + (PAGE_SIZE - 1);

			L("%-6s | %10u | %10u | %7u | 0x%016lx - 0x%016lx",
					last_status ? "USED" : "FREE",
					(uint32_t)start_page, (uint32_t)end_page,
					(uint32_t)count, (uint64_t)start_addr,
					(uint64_t)end_addr);

			start_page  = i;
			last_status = current_status;
		}
	}

	L("--- End of PMM Dump ---");
}

/**
 * pmm_print_stats() - 打印当前内存统计信息。
 *
 * 直接读 pinfo.page 计数，不扫 bitmap:pinfo 在每次 alloc/free
 * 时由 pmm_account_alloc / pmm_account_free 维护，是 O(1) 读取。
 *
 * 历史版本(已删)做了一次全扫 bitmap (O(n))，再内嵌 pmm_dump_bitmap()
 * 又扫一遍 (O(n))，并在 pmm_init 阶段被调用 4 次：合计 8 次全扫，
 * SMP 启动后还会与并发 alloc/free 撕裂数据。现在 O(1) + 持锁。
 *
 * pmm_dump_bitmap 保留为独立函数，需要详细位图分布时调用方显式调。
 */
void pmm_print_stats(void)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&pinfo.pmm_lock, flags);

	uint64_t usable_pages = pinfo.page.usable;
	uint64_t free_pages = pinfo.page.freed;
	uint64_t self_pages = pinfo.page.self;
	uint64_t used_pages = usable_pages - free_pages;

	arch_spin_unlock_irqrestore(&pinfo.pmm_lock, flags);

	/* 换算为最合适的单位 */
#define TO_MB(pages) (((pages) * PAGE_SIZE) >> 20)

	L("PMM Stats:");
	L("  Usable : %4lu MB  (%lu pages)",  TO_MB(usable_pages), usable_pages);
	L("  Used   : %4lu MB  (%lu pages)",  TO_MB(used_pages),   used_pages);
	L("  Free   : %4lu MB  (%lu pages)",  TO_MB(free_pages),   free_pages);
	L("  Bitmap : %4lu MB  (%lu pages)",  TO_MB(self_pages),   self_pages);

#undef TO_MB
}

char *MEMMAP_STR[] = {
	"Usable",
	"Reserved",
	"ACPI_RECLAIMABLE",
	"ACPI_NVS",
	"BAD_MEMORY",
	"BOOTLOADER_RECLAIMABLE",
	"EXECUTABLE_AND_MODULES",
	"FRAMEBUFFER",
	"ACPI_TABLES",
};

inline char *mmstr(int type)
{
	return MEMMAP_STR[type];
}
