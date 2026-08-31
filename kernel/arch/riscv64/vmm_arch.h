/* vmm_arch.h — RISC-V 64 页表标志位 (stub) */
#ifndef __VMM_ARCH_RISCV64_H__
#define __VMM_ARCH_RISCV64_H__

#include <stdint.h>

/* RISC-V Sv39 页表 (stub — 待填充) */
#define PTE_PRESENT       (1ULL << 0)
#define PTE_WRITABLE      (1ULL << 1)
#define PTE_USER          (1ULL << 2)
#define PTE_WRITE_THROUGH 0
#define PTE_CACHE_DISABLE 0
#define PTE_HUGE          0
#define PTE_GLOBAL        0
#define PTE_NX            (1ULL << 63)
#define PTE_ADDR_MASK     0x000ffffffffff000ULL

#define PT_ROOT_IDX(v)  (((v) >> 39) & 0x1FF)
#define PT_LVL1_IDX(v)  (((v) >> 30) & 0x1FF)
#define PT_LVL2_IDX(v)  (((v) >> 21) & 0x1FF)
#define PT_LVL3_IDX(v)  (((v) >> 12) & 0x1FF)

#define PML4_IDX(v) PT_ROOT_IDX(v)
#define PDPT_IDX(v) PT_LVL1_IDX(v)
#define PD_IDX(v)   PT_LVL2_IDX(v)
#define PT_IDX(v)   PT_LVL3_IDX(v)

static inline void arch_write_pt_root(uint64_t phys)
{
	(void)phys;
	/* stub */
}

#define arch_write_cr3(phys) arch_write_pt_root(phys)

#endif
