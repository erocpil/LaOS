#ifndef __VMM_H__
#define __VMM_H__

/*
 * vmm.h - 4 级页表管理接口
 */

#include <stdint.h>
#include <stdbool.h>

#include "arch_dispatch.h"

/* 标志位说明
 * 具体值由 arch/<arch>/vmm_arch.h 定义，各架构 PTE 位布局不同。
 * x86_64: PTE_PRESENT=bit0, PTE_WRITABLE=bit1, PTE_USER=bit2, PTE_NX=bit63
 * aarch64: PTE_PRESENT=bit0, PTE_WRITABLE=bit7(AP[1]), PTE_USER=bit6(AP[2]), PTE_NX=bit54(UXN)
 *
 * 所有架构共用以下语义接口：
 *   PTE_PRESENT / PTE_WRITABLE / PTE_USER / PTE_NX / PTE_ADDR_MASK
 *   PT_ROOT_IDX / PT_LVL1_IDX / PT_LVL2_IDX / PT_LVL3_IDX
 *   PML4_IDX / PDPT_IDX / PD_IDX / PT_IDX  (兼容旧名)
 *   arch_write_pt_root(phys)
 */

extern uint64_t *kernel_pml4;

void vmm_init(int flag);
void vmm_test(void);
void vmm_test_secret_base(void);
void vmm_test_framebuffer(void *fb);
int vmm_map(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);
int vmm_map_global(uint64_t vaddr, uint64_t paddr, uint64_t flags);
int vmm_remap(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);
int vmm_map_region(uint64_t *pml4, uint64_t vaddr, uint64_t paddr,
		uint64_t size, uint64_t flags);
int vmm_map_user(uint64_t *pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags);
void vmm_map_specific(uint64_t* pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags);
int vmm_unmap(uint64_t* pml4, uint64_t vaddr);
uint64_t *vmm_create_user_pml4(void);
void vmm_dump_pml4(uint64_t pml4_phys, const char *name);
int vmm_is_mapped(uint64_t pml4_phys, uint64_t vaddr);
void vmm_destroy_level(uint64_t table_phys, int level);
uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t vaddr);

static inline uint64_t *get_kernel_pagemap(void)
{
	extern uint64_t *kernel_pml4;
	return kernel_pml4;
}

#endif
