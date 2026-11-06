#ifndef __VMM_H__
#define __VMM_H__

/*
 * vmm.h — 页表管理接口
 */

#include <stdint.h>
#include <stdbool.h>

#include "arch_dispatch.h"

/* 页表标志的具体位定义由 arch/<arch>/vmm_arch.h 提供。通用代码使用以下
 * 架构无关的语义接口：
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
int vmm_remap(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);
int vmm_map_region(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags);
int vmm_map_user(uint64_t *pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags);
void vmm_map_specific(uint64_t *pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags);
int vmm_unmap(uint64_t *pml4, uint64_t vaddr);
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
