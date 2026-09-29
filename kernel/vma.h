/*
 * vma.h — Virtual Memory Area tracking
 *
 * Per-thread linked list of mapped regions. Used by mmap/munmap
 * to track user virtual address space allocations.
 */

#ifndef __VMA_H__
#define __VMA_H__

#include <stdint.h>

/* thread.h forward-declares struct vma; we need struct thread here. */
#include "thread.h"

/* Protection flags (mmap prot) */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

/* Mapping flags (mmap flags) */
#define MAP_ANONYMOUS 0x20
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_LAZY      0x100 /* VMA only — no physical page allocation */

struct vma {
	uint64_t start; /* virtual address start (page-aligned) */
	uint64_t end; /* virtual address end (page-aligned, exclusive) */
	uint32_t prot; /* PROT_READ | PROT_WRITE | PROT_EXEC */
	uint32_t flags; /* MAP_ANONYMOUS | MAP_PRIVATE etc. */
	struct vma *next;
};

/* Allocate a new VMA and insert into thread's VMA list (sorted by start).
 * Returns the VMA, or NULL on OOM. */
struct vma *vma_alloc(struct thread *t, uint64_t start, uint64_t end,
		uint32_t prot, uint32_t flags);

/* Find the VMA containing addr in thread's list. Returns NULL if none. */
struct vma *vma_find(struct thread *t, uint64_t addr);

/* Remove and free the VMA covering [start, start+length) from thread's list.
 * Returns 0 on success, -1 if the range isn't fully covered by one VMA. */
int vma_free(struct thread *t, uint64_t start, uint64_t length);

/* Free all VMAs in thread's list (called on thread exit). */
void vma_destroy_all(struct thread *t);

/* Find a free virtual address range of at least `length` bytes in the
 * user address space. Returns the start address, or 0 if no space. */
uint64_t vma_find_free(uint64_t length);

#endif /* __VMA_H__ */
