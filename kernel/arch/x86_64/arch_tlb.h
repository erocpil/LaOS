#ifndef __ARCH_TLB_H__
#define __ARCH_TLB_H__

#include <stdint.h>

#include "hhdm.h"

extern uint64_t *kernel_pml4;

/*
 * x86_64 TLB / CR3 操作的 arch 包装。
 *
 * 命名照 Linux arch/x86/include/asm/tlbflush.h 风格：
 *   arch_tlb_flush_one(vaddr)  单页 invlpg
 *   arch_tlb_flush_all()       全 TLB flush(写回 cr3 触发)
 *   arch_read_cr3()            读 cr3
 *   arch_write_cr3(val)        写 cr3(隐含全 TLB flush)
 *
 * 设计说明：
 * 1. arch_tlb_flush_all 用 "mov %%cr3, %%rax; mov %%rax, %%cr3" 的旧式做法触发
 *    刷新；LaOS 当前不启用 cr4.PGE(全局页)，无需走 cr4 toggle 方案。
 * 2. arch_write_cr3 本身也会全 TLB flush，与 arch_tlb_flush_all 语义重叠；
 *    保留两者是为了表达调用方意图：换页表 vs 单纯刷 TLB.
 * 3. PCID / INVPCID 暂不支持，YAGNI.
 */

static inline void arch_tlb_flush_one(uintptr_t vaddr)
{
	__asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static inline void arch_tlb_flush_all(void)
{
	__asm__ volatile("mov %%cr3, %%rax\n\t"
	                 "mov %%rax, %%cr3"
	                 ::: "rax", "memory");
}

static inline uint64_t arch_read_cr3(void)
{
	uint64_t cr3;

	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

	return cr3;
}

static inline void arch_write_cr3(uint64_t val)
{
	__asm__ volatile("mov %0, %%cr3" : : "r"(val) : "memory");
}

static inline void arch_switch_page_table_root(uint64_t root_phys)
{
	arch_write_cr3(root_phys);
}

static inline uint64_t arch_kernel_root_phys(void)
{
	return virt_to_phys(kernel_pml4);
}

/** arch_user_elf_load_begin — 切换到用户 CR3，返回旧值
 *  arch_user_elf_load_end   — 恢复内核 CR3 */
static inline uint64_t arch_user_elf_load_begin(uint64_t pml4_phys)
{
	uint64_t old = arch_read_cr3();
	arch_write_cr3(pml4_phys & ~0xFFFULL);
	return old;
}

static inline void arch_user_elf_load_end(uint64_t token)
{
	arch_write_cr3(token);
}

#endif /* __ARCH_TLB_H__ */
