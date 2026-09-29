#ifndef __HEAP_H__
#define __HEAP_H__

/*
 * heap.h - Heap 分配器 kheap 契约
 */

#include <stdint.h>
#include <stddef.h>

#include "lock.h"
#include "list.h"

// 地址布局见 module_alloc.h：kernel(0xffffffff80..)-module(0xffffffffc0..)
// kheap(0xffffa0..)，三者不重叠，kheap 独占 1GB 线性增长空间。
#define KHEAP_MAGIC 0x48454150 // "HEAP"
#define HEAP_MAX_SIZE (1ULL << 30) // 1GB
#define HEAP_MALLOC_FROM_TAIL

extern struct kheap kheap_g;

/**
 * kheap_node — 堆块头部（48 字节，16 字节对齐）。
 *
 * sizeof(kheap_node) 必须是 16 的倍数。因为 kmalloc 返回
 * next + sizeof(kheap_node)，而 next 本身在 16 字节对齐地址上，
 * 只有 header 大小也是 16 的倍数才能保证返回指针 16 字节对齐。
 *
 * fxsave64/fxrstor64 要求目标地址 16 字节对齐，不满足则 #GP(0)。
 *
 * 字段：node(16) + magic(4) + is_free(4) + size(8) + id(8) = 40。
 * 补 _pad(8) → 48，保证 kmalloc 返回 16 字节对齐。
 */
struct kheap_node {
	struct list_node node; // 16: next + prev 指针
	uint32_t magic;        //  4: KHEAP_MAGIC
	uint32_t is_free;      //  4: 空闲标志
	size_t size;           //  8: 块总大小（含 header）
	int64_t id;            //  8: 分配线程 ID（调试用）
	uint64_t _pad;         //  8: 对齐填充 → sizeof = 48 = 16×3
};

struct kheap {
	struct kheap_node head;
	spinlock_t lock;
	uintptr_t base;
	// 当前已映射的物理内存边界
	uintptr_t max;
	uint64_t count;
};

void kheap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
void kheap_stats(void);
void kheap_test(void);

#define DUMP_KHEAP_NODE(n) \
	do { \
		__typeof__(n) _n = (n); \
		L("vm node %p %p", _n, (char*)(_n) + sizeof(struct kheap_node)); \
		L("  magic 0x%x", _n->magic); \
		L("  is_free %d", _n->is_free); \
		L("  size %lu", _n->size); \
		L("  id %ld", _n->id); \
		L("  node %p %p", _n->node.prev, _n->node.next); \
	} while (0)
#endif
