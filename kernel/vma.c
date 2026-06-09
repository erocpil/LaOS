/*
 * vma.c — VMA linked-list management
 *
 * Per-thread sorted linked list of virtual memory areas.
 * Protects user address space from overlap and enables munmap.
 */

#include "vma.h"
#include "thread.h"
#include "heap.h"
#include "cpu.h"
#include "string.h"

/* User mmap allocation starts here (above ELF load area at 0x400000).
 * Must avoid PCI MMIO (0x10000000-0x3fffffff on QEMU virt, highmem=off). */
#define USER_MMAP_BASE 0x04000000ULL
#define USER_MMAP_END  0x10000000ULL /* below PCI MMIO */

/* VMA list operations */

struct vma *vma_alloc(struct thread *t, uint64_t start, uint64_t end,
		uint32_t prot, uint32_t flags)
{
	struct vma *v = kmalloc(sizeof(*v));
	if (!v) {
		return NULL;
	}
	memset(v, 0, sizeof(*v));
	v->start = start;
	v->end   = end;
	v->prot  = prot;
	v->flags = flags;

	/* Insert sorted by start address */
	struct vma **pp = &t->vma_list;
	while (*pp && (*pp)->start < start) {
		pp = &(*pp)->next;
	}
	v->next = *pp;
	*pp = v;

	return v;
}

struct vma *vma_find(struct thread *t, uint64_t addr)
{
	for (struct vma *v = t->vma_list; v; v = v->next) {
		if (addr >= v->start && addr < v->end) {
			return v;
		}
	}

	return NULL;
}

int vma_free(struct thread *t, uint64_t start, uint64_t length)
{
	uint64_t end = start + length;
	struct vma **pp = &t->vma_list;

	while (*pp) {
		struct vma *v = *pp;
		if (v->start == start && v->end == end) {
			*pp = v->next;
			kfree(v);
			return 0;
		}
		pp = &v->next;
	}

	return -1; /* no exact match */
}

void vma_destroy_all(struct thread *t)
{
	struct vma *v = t->vma_list;
	while (v) {
		struct vma *next = v->next;
		kfree(v);
		v = next;
	}
	t->vma_list = NULL;
}

uint64_t vma_find_free(uint64_t length)
{
	/* Round up to page size */
	length = (length + 4095) & ~4095ULL;

	/* Uses the current CPU's running thread's VMA list. */
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *t = ctx->current;
	if (!t) {
		return 0;
	}

	uint64_t cursor = USER_MMAP_BASE;

	for (struct vma *v = t->vma_list; v; v = v->next) {
		if (cursor + length <= v->start && cursor < USER_MMAP_END) {
			return cursor;
		}
		cursor = v->end;
		if (cursor >= USER_MMAP_END) {
			return 0;
		}
	}

	/* Check after last VMA */
	if (cursor + length <= USER_MMAP_END) {
		return cursor;
	}

	return 0;
}
