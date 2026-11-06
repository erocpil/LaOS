/*
 * vma.c — VMA 链表管理
 *
 * 每个线程按起始地址维护已映射区域链表，用于跟踪用户地址空间分配并支持
 * munmap。
 */

#include "vma.h"
#include "thread.h"
#include "heap.h"
#include "cpu.h"
#include "string.h"

/* mmap 分配区位于 ELF 加载区上方，并在当前 QEMU virt 配置下避开 PCI MMIO 区域。 */
#define USER_MMAP_BASE 0x04000000ULL
#define USER_MMAP_END  0x10000000ULL /* below PCI MMIO */

/* VMA 链表操作。 */

struct vma *vma_alloc(struct thread *t, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags)
{
	struct vma *v = kmalloc(sizeof(*v));
	if (!v) {
		return NULL;
	}
	memset(v, 0, sizeof(*v));
	v->start = start;
	v->end = end;
	v->prot = prot;
	v->flags = flags;

	/* 按起始地址升序插入。 */
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

	return -1; /* 未找到起止地址完全匹配的 VMA。 */
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
	/* 将长度向上取整到页边界。 */
	length = (length + 4095) & ~4095ULL;

	/* 使用当前 CPU 上运行线程的 VMA 链表。 */
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

	/* 检查最后一个 VMA 之后的地址范围。 */
	if (cursor + length <= USER_MMAP_END) {
		return cursor;
	}

	return 0;
}
