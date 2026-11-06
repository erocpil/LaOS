/*
 * vma.h — 虚拟内存区域（VMA）管理接口
 *
 * 每个线程维护一条已映射区域的链表，用于跟踪用户地址空间中的 mmap/munmap
 * 分配。
 */

#ifndef __VMA_H__
#define __VMA_H__

#include <stdint.h>

/* thread.h 前置声明了 struct vma；此处需要完整的 struct thread 定义。 */
#include "thread.h"

/* mmap 保护标志。 */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

/* mmap 映射标志。 */
#define MAP_ANONYMOUS 0x20
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_LAZY      0x100 /* VMA only — no physical page allocation */

struct vma {
	uint64_t start; /* virtual address start (page-aligned) */
	uint64_t end;	/* virtual address end (page-aligned, exclusive) */
	uint32_t prot;	/* PROT_READ | PROT_WRITE | PROT_EXEC */
	uint32_t flags; /* MAP_ANONYMOUS | MAP_PRIVATE etc. */
	struct vma *next;
};

/* 分配 VMA 并按起始地址插入线程链表；内存不足时返回 NULL。 */
struct vma *vma_alloc(
	struct thread *t, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags);

/* 查找包含 addr 的 VMA；未找到时返回 NULL。 */
struct vma *vma_find(struct thread *t, uint64_t addr);

/* 移除并释放覆盖 [start, start+length) 的 VMA；成功返回 0，否则返回 -1。 */
int vma_free(struct thread *t, uint64_t start, uint64_t length);

/* 释放线程链表中的全部 VMA，在线程退出时调用。 */
void vma_destroy_all(struct thread *t);

/* 在用户地址空间查找至少 length 字节的空闲范围；未找到时返回 0。 */
uint64_t vma_find_free(uint64_t length);

#endif /* __VMA_H__ */
