/*
 * vmm_arch.h — x86_64 页表标志位与操作
 *
 * x86_64 使用 4-level PML4 页表，PTE 格式：
 *   bit 0:   Present
 *   bit 1:   Read/Write
 *   bit 2:   User/Supervisor
 *   bit 3:   PWT (Write-Through)
 *   bit 4:   PCD (Cache Disable)
 *   bit 7:   PS (Page Size — 1GB/2MB huge page leaf)
 *   bit 8:   Global
 *   bit 63:  NX (No-Execute)
 *
 * 地址掩码：bits [51:12]
 */

#ifndef __VMM_ARCH_X86_64_H__
#define __VMM_ARCH_X86_64_H__

#include <stdint.h>

#define PTE_PRESENT          (1ULL << 0)
#define PTE_WRITABLE         (1ULL << 1)
#define PTE_USER             (1ULL << 2)
#define PTE_WRITE_THROUGH    (1ULL << 3)
#define PTE_CACHE_DISABLE    (1ULL << 4)
#define PTE_HUGE             (1ULL << 7)
#define PTE_GLOBAL           (1ULL << 8)
#define PTE_NX               (1ULL << 63)
#define PTE_ADDR_MASK        0x000ffffffffff000ULL

/* x86_64: present + PS bit → huge page leaf */
#define PTE_IS_LEAF(e)      (((e) & (PTE_PRESENT | PTE_HUGE)) == (PTE_PRESENT | PTE_HUGE))

/* x86_64 has no MAIR/AttrIndx; PTE_PCD/PTE_PWT control caching. */
#define PTE_MEMATTR_NORMAL 0

/* x86_64 无需额外 leaf 标志 */
#define VMM_LEAF_EXTRA_FLAGS 0

/* x86_64 PTE_USER=bit2 不在地址域，表描述符可安全继承 */
#define PTE_TABLE_USER_MASK(f)  ((f) & PTE_USER)

/* 虚拟地址拆分：4-level 页表每级 9-bit 索引 */
#define PT_ROOT_IDX(v)  (((v) >> 39) & 0x1FF)   /* PML4 */
#define PT_LVL1_IDX(v)  (((v) >> 30) & 0x1FF)   /* PDPT */
#define PT_LVL2_IDX(v)  (((v) >> 21) & 0x1FF)   /* PD   */
#define PT_LVL3_IDX(v)  (((v) >> 12) & 0x1FF)   /* PT   */

/* 兼容旧名 */
#define PML4_IDX(v) PT_ROOT_IDX(v)
#define PDPT_IDX(v) PT_LVL1_IDX(v)
#define PD_IDX(v)   PT_LVL2_IDX(v)
#define PT_IDX(v)   PT_LVL3_IDX(v)

/* x86_64 无额外的表描述符位需求 */
#define VMM_TABLE_EXTRA_FLAGS 0

static inline void arch_write_pt_root(uint64_t phys)
{
	__asm__ volatile("mov %0, %%cr3" : : "r"(phys) : "memory");
}

/* 兼容旧名 — vmm.c 使用 arch_write_cr3 */
#define arch_write_cr3(phys) arch_write_pt_root(phys)

#endif /* __VMM_ARCH_X86_64_H__ */
