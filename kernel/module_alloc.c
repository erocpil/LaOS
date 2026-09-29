/*
 * module_alloc: 给内核模块加载缓冲区分配 -2GB 区虚拟地址。
 * 详细背景见 module_alloc.h.
 *
 * 实现:bump 指针 + 按页 pmm_alloc + vmm_map.
 * 并发：模块加载频率低，单一 spinlock 串行化所有分配。
 */

#include "pmm.h"
#include "vmm.h"
#include "lock.h"
#include "define.h"
#include "hhdm.h"
#include "debug.h"
#include "module_alloc.h"
#include "log.h"
#include "arch_tlb.h"

static spinlock_t module_lock;
static uintptr_t module_brk; /* 下一次 bump 落点 */
static uintptr_t module_mapped; /* 已映射上界 (含)，按 PAGE_SIZE 对齐 */
static uintptr_t module_base;
static uintptr_t module_max;

/**
 * module_expand_to() - 把 [module_mapped, target_aligned) 区间按 4KB
 * 申请物理页并映射进内核页表。
 *
 * 调用方持锁。
 */
static bool module_expand_to(uintptr_t target)
{
	uintptr_t target_aligned = (target + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);

	while (module_mapped < target_aligned) {
		/*
		 * 同 module_alloc 的上限判断:MODULE_VBASE + MODULE_SIZE 会回绕到 0，
		 * 改用 module_mapped 与 MODULE_VMAX 直接比较。
		 */
		if (module_mapped > module_max - (PAGE_SIZE - 1)) {
			L("module_alloc: VA region exhausted at 0x%lx", module_mapped);
			return false;
		}
		void *phys = pmm_alloc();
		if (!phys) {
			L("module_alloc: pmm_alloc failed at 0x%lx", module_mapped);
			return false;
		}
		vmm_map(kernel_pml4, module_mapped, (uintptr_t)phys,
				PTE_PRESENT | PTE_WRITABLE);
		arch_tlb_flush_one(module_mapped);
		module_mapped += PAGE_SIZE;
	}

	return true;
}

void module_alloc_init(void)
{
	module_base = arch_module_alloc_base();
	module_max = module_base + MODULE_REGION_SIZE - 1;
	spin_lock_init(&module_lock);
	module_brk = module_base;
	module_mapped = module_base;
	L_TAG(LOG_MODULE, "module_alloc: VA region 0x%lx - 0x%lx (%lu MB)\n",
			(uint64_t)module_base, (uint64_t)module_max,
			(uint64_t)MODULE_REGION_SIZE >> 20);
}

uintptr_t module_region_base(void) { return module_base; }
EXPORT_SYMBOL(module_region_base);
uintptr_t module_region_max(void) { return module_max; }

bool module_region_contains(uintptr_t address)
{
	return address >= module_base && address <= module_max;
}

struct module_alloc_checkpoint module_alloc_checkpoint(void)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&module_lock, flags);

	struct module_alloc_checkpoint checkpoint = {
		.brk = module_brk,
		.mapped = module_mapped,
	};

	arch_spin_unlock_irqrestore(&module_lock, flags);

	return checkpoint;
}

void module_alloc_rollback(struct module_alloc_checkpoint checkpoint)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&module_lock, flags);

	if (checkpoint.brk < module_base || checkpoint.brk > module_brk ||
			checkpoint.mapped < module_base ||
			checkpoint.mapped > module_mapped ||
			checkpoint.mapped < checkpoint.brk) {
		arch_spin_unlock_irqrestore(&module_lock, flags);
		L("module_alloc: invalid rollback brk=0x%lx mapped=0x%lx current brk=0x%lx mapped=0x%lx",
				(uint64_t)checkpoint.brk, (uint64_t)checkpoint.mapped,
				(uint64_t)module_brk, (uint64_t)module_mapped);
		return;
	}

	uintptr_t unmap_start = checkpoint.mapped;
	uintptr_t unmap_end = module_mapped;
	module_brk = checkpoint.brk;
	module_mapped = checkpoint.mapped;

	for (uintptr_t va = unmap_start; va < unmap_end; va += PAGE_SIZE) {
		uint64_t phys = vmm_get_phys(virt_to_phys(kernel_pml4), va);
		if (phys) {
			vmm_unmap(kernel_pml4, va);
			pmm_free((void *)phys);
		}
	}

	arch_spin_unlock_irqrestore(&module_lock, flags);

	L_TAG(LOG_MODULE, "module_alloc rollback: brk=0x%lx mapped=0x%lx\n",
			(uint64_t)checkpoint.brk, (uint64_t)checkpoint.mapped);
}

void *module_alloc(size_t size)
{
	if (size == 0) {
		return NULL;
	}

	/* 16 字节对齐分配粒度 */
	size_t need = (size + 15) & ~(size_t)15;

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&module_lock, flags);

	uintptr_t addr = module_brk;

	/*
	 * 上限判断必须避免 MODULE_VBASE + MODULE_SIZE 在 64 位算术里溢出到 0.
	 * MODULE_VMAX = 0xffffffffffffffff (含)，所以剩余空间 = MODULE_VMAX - addr + 1，
	 * 但 +1 也可能溢出，故改写成 need - 1 与 (MODULE_VMAX - addr) 比较。
	 */
	if (need == 0 || need - 1 > module_max - addr) {
		arch_spin_unlock_irqrestore(&module_lock, flags);
		L("module_alloc: size %lu overflows region (brk=0x%lx, remaining=%lu)",
				size, (uint64_t)addr, (uint64_t)(module_max - addr + 1));
		return NULL;
	}

	uintptr_t next = addr + need;

	if (!module_expand_to(next)) {
		arch_spin_unlock_irqrestore(&module_lock, flags);
		return NULL;
	}

	module_brk = next;
	arch_spin_unlock_irqrestore(&module_lock, flags);

	return (void*)addr;
}

void module_free(void *ptr)
{
	/* bump 分配器不回收。保留接口供未来真实 unload 实现替换。 */
	(void)ptr;
}

void module_alloc_stats(void)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&module_lock, flags);

	uintptr_t used = module_brk - module_base;
	uintptr_t mapped = module_mapped - module_base;

	arch_spin_unlock_irqrestore(&module_lock, flags);

	L("module_alloc: used=%lu KB mapped=%lu KB region=%lu MB",
			(uint64_t)used >> 10, (uint64_t)mapped >> 10,
			(uint64_t)MODULE_REGION_SIZE >> 20);
}
