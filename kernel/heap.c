/*
 * heap.c - 内核堆分配器(kheap)
 *
 * 基于链表的 first-fit 分配器，释放时按物理邻接判定合并。
 * 仅用于内核控制面的小块分配(线程栈，TCB，节点结构)。
 */
#include "pmm.h"
#include "config.h"
#include "vmm.h"
#include "heap.h"
#include "hhdm.h"
#include "string.h"
#include "export.h"
#include "define.h"
#include "debug.h"
#include "log.h"
#include "arch_tlb.h"

struct kheap kheap_g;

// 内部函数：确保虚拟地址到 target_addr 都有物理页映射
static bool kheap_expand_to_addr(uintptr_t target_addr)
{
	/* P0-9: guard against alignment overflow */
	if (target_addr > UINTPTR_MAX - 4095)
		return false;
	uintptr_t target_aligned = (target_addr + 4095) & ~4095;

	while (kheap_g.max < target_aligned) {
		if (kheap_g.max + PAGE_SIZE > kheap_g.base + HEAP_MAX_SIZE) {
			L();
			return false;
		}

		/* 若已被 larger mapping 覆盖（如 ARM64 2MB block），跳过 vmm_map */
		if (!vmm_is_mapped(virt_to_phys(kernel_pml4), kheap_g.max)) {
			void* phys = pmm_alloc();
			if (!phys) {
				return false;
			}

			// 必须映射到内核页表
			if (vmm_map(kernel_pml4, kheap_g.max, (uintptr_t)phys,
				   PTE_PRESENT | PTE_WRITABLE) != 0) {
				pmm_free(phys);
				return false;
			}
			arch_tlb_flush_one(kheap_g.max);
		}
		kheap_g.max += PAGE_SIZE;
	}

	return true;
}

static void kheap_node_set_id(struct kheap_node *vmn)
{
	struct cpu_context *ctx = NULL;

	vmn->id = 0;
	ctx = cpu_get_ctx();

	if (ctx && ctx->current) {
		vmn->id = ctx->current->id;
	}
}

void *kmalloc(size_t size)
{
	if (size == 0) {
		return NULL;
	}

	// 16 字节对齐并包含 header 空间
	/* P0-9: prevent overflow when size is near SIZE_MAX */
	if (size > SIZE_MAX - sizeof(struct kheap_node) - 15)
		return NULL;
	size_t total_needed = (size + sizeof(struct kheap_node) + 15) & ~15;

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&kheap_g.lock, flags);

	// First-fit 查找空闲块
	struct kheap_node *curr = NULL;
#ifdef HEAP_MALLOC_FROM_TAIL
	list_for_each_entry(curr, &kheap_g.head.node, node) {
		if (curr->is_free && curr->size >= total_needed) {
			struct kheap_node *next = curr;
			if (curr->size >= sizeof(struct kheap_node) + total_needed + 16) {
				curr->size = curr->size - total_needed;
				next = (struct kheap_node*)((uintptr_t)curr + curr->size);
				list_init(&next->node);
				list_add(&next->node, &curr->node);
				L("split");
			}
			L("size enough");
			next->magic = KHEAP_MAGIC;
			next->size = total_needed;
			next->is_free = false;
			kheap_node_set_id(next);
			arch_spin_unlock_irqrestore(&kheap_g.lock, flags);
			return (void*)((uintptr_t)next + sizeof(struct kheap_node));
		}
	}
#else
	list_for_each_entry(curr, &kheap_g.head.node, node) {
		if (curr->is_free && curr->size >= total_needed) {
			struct kheap_node *next = curr;
			if (curr->size >= sizeof(struct kheap_node) + total_needed + 16) {
				curr = (struct kheap_node*)((uintptr_t)curr + total_needed);
				curr->size = next->size - total_needed;
				curr->is_free = true;
				next->id = -1;
				list_init(&curr->node);
				list_add(&curr->node, &next->node);
				L("split");
			}
			L("size enough");
			next->magic = KHEAP_MAGIC;
			next->size = total_needed;
			next->is_free = false;
			kheap_node_set_id(next);
			arch_spin_unlock_irqrestore(&kheap_g.lock, flags);
			return (void*)((uintptr_t)next + sizeof(struct kheap_node));
		}
	}
#endif

	L("new block");
	struct kheap_node *last = list_last_entry(&kheap_g.head.node, struct kheap_node, node);
	DUMP_KHEAP_NODE(last);
	// 没找到，扩容并在末尾创建
	uintptr_t new_node_addr = (uintptr_t)last + last->size;
	if (!kheap_expand_to_addr(new_node_addr + total_needed)) {
		L("cannot kmalloc expand 0x%lx + %lu", new_node_addr, total_needed);
		arch_spin_unlock_irqrestore(&kheap_g.lock, flags);
		return NULL;
	}

	struct kheap_node *vmn = (struct kheap_node*)new_node_addr;
	list_init(&vmn->node);
	vmn->magic = KHEAP_MAGIC;
	vmn->size = total_needed;
	vmn->is_free = false;
	kheap_node_set_id(vmn);
	list_add(&vmn->node, &last->node);

	arch_spin_unlock_irqrestore(&kheap_g.lock, flags);

	void *va = (void*)((uintptr_t)vmn + sizeof(struct kheap_node));

	return va;
}
EXPORT_SYMBOL(kmalloc);

void kfree(void* ptr)
{
	if (!ptr) {
		return;
	}

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&kheap_g.lock, flags);
	struct kheap_node *curr = (struct kheap_node*)((uintptr_t)ptr - sizeof(struct kheap_node));
	if (curr->is_free) {
		DUMP_KHEAP_NODE(curr);
		panic("kfree: double free at %p", ptr);
	}
#if CONFIG_DEBUG
	DUMP_KHEAP_NODE(curr);
#endif

	if (curr->magic != KHEAP_MAGIC) {
		panic("kfree: invalid magic at %p (expected 0x%x, got 0x%x)",
				ptr, KHEAP_MAGIC, curr->magic);
	}

	struct kheap_node *prev = list_last_entry(&curr->node, struct kheap_node, node);
	struct kheap_node *next = list_first_entry(&curr->node, struct kheap_node, node);

	if ((char*)curr + sizeof(struct kheap_node) != ptr) {
		panic("kfree: payload mismatch curr=%p ptr=%p", curr, ptr);
	}
	L("Freeing %p -> %p prev %p -> %p next %p -> %p",
			curr, (char*)curr + sizeof(struct kheap_node),
			prev, (char*)prev + sizeof(struct kheap_node),
			next, (char*)next + sizeof(struct kheap_node));

	/*
	 * Merge 前必须验证 list 邻居在物理上确实与 curr 相邻。
	 * kheap 链表用 list_add(&next->node, &curr->node) 插入，
	 * 不维护"链表顺序 = 物理顺序"不变式，长时间 alloc/free 后
	 * 链表邻居与物理邻居可能漂移。盲合并会跨过中间仍在使用的块。
	 *
	 * 物理邻接公式(size 字段包含 sizeof(kheap_node) header):
	 *   prev 紧邻 curr <-> (uintptr_t)prev + prev->size == (uintptr_t)curr
	 *   curr 紧邻 next <-> (uintptr_t)curr + curr->size == (uintptr_t)next
	 */
	bool prev_adj = ((uintptr_t)prev + prev->size == (uintptr_t)curr);
	bool next_adj = ((uintptr_t)curr + curr->size == (uintptr_t)next);

	if (prev->is_free && prev_adj) {
		prev->size += curr->size;
		list_del_init(&curr->node);
		curr = prev;
		L("merge prev");
	} else if (prev->is_free && !prev_adj) {
		L("skip merge prev: not physically adjacent (prev=%p+%lu vs curr=%p)",
				prev, prev->size, curr);
	}

	if (next->is_free && next_adj) {
		curr->size += next->size;
		list_del_init(&next->node);
		L("merge next");
	} else if (next->is_free && !next_adj) {
		L("skip merge next: not physically adjacent (curr=%p+%lu vs next=%p)",
				curr, curr->size, next);
	}

	curr->is_free = true;
	curr->id = 0;

	L("freed ptr %p", ptr);

	arch_spin_unlock_irqrestore(&kheap_g.lock, flags);
}

static inline void kheap_node_append(struct kheap_node *n, struct kheap *h)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&h->lock, flags);

	list_add_tail(&n->node, &h->head.node);

	arch_spin_unlock_irqrestore(&h->lock, flags);
}

/**
 * 在 x86_64 架构中，地址线目前通常只有 48 位。
 * 为了保证兼容性，地址必须是"规范的"，即第 47 位的值必须扩展到第 48 到 63 位。
 * 用户空间：通常位于 0x0000000000000000 到 0x00007fffffffffff.
 * 内核空间：通常位于 0xffff800000000000 到 0xffffffffffffffff.
 * 0xffffa00000000000 处于内核空间的安全区域。它避开了 Limine 默认映射内核代码的
 * 区域(通常在 0xffffffff80000000 附近)，为堆分配留出了巨大的线性增长空间。
 */
void kheap_init()
{
	memset(&kheap_g, 0, sizeof(kheap_g));
	spin_lock_init(&kheap_g.lock);
	kheap_g.base = KHEAP_VBASE;
	kheap_g.max = KHEAP_VBASE;
	kheap_g.count = 1;
	struct kheap_node *init = &kheap_g.head;
	init->magic = KHEAP_MAGIC;
	init->size = 0;
	init->is_free = false;
	init->id = 0;
	INIT_LIST_NODE(&kheap_g.head.node);
	L("init");

	// 初始映射 1 页并建立第一个大空闲块
	kheap_expand_to_addr(kheap_g.base + sizeof(struct kheap_node) + PAGE_SIZE);

	struct kheap_node *vmn = (struct kheap_node*)kheap_g.base;
	INIT_LIST_NODE(&vmn->node);
	vmn->magic = KHEAP_MAGIC;
	vmn->size = PAGE_SIZE;
	vmn->is_free = true;
	vmn->id = 0;
	kheap_node_append(vmn, &kheap_g);
	L("first vmn %p node %p", vmn, &vmn->node);

	L_TAG(LOG_HEAP, "Initialized kheap with linked list at %p.\n", (void*)kheap_g.base);
}

void kheap_stats()
{
	uint64_t flags = 0;

	arch_spin_lock_irqsave(&kheap_g.lock, flags);
	L("--- Heap Stats ---");
	int i = 0;
	struct kheap_node *curr = NULL;
	list_for_each_entry(curr, &kheap_g.head.node, node) {
		L("Block %d: %p -> 0x%08x | Size: %lu | Free: %s | Id: %ld",
				i++, curr, (char*)curr + sizeof(struct kheap_node),
				curr->size, curr->is_free ? "YES" : "NO", curr->id);
	}

	arch_spin_unlock_irqrestore(&kheap_g.lock, flags);
}

static void kheap_node_set_id_at(void *ptr, uint64_t id)
{
	struct kheap_node *n = (struct kheap_node*)((char*)ptr - sizeof(struct kheap_node));
	n->id = id;
}

static void test_heap_recycle()
{
	L("Testing heap recycling...");
	void *ptrs[5];

	kheap_stats();

	// 1. 分配 5 个块
	L("kmalloc 5 blocks:");
	for (int i = 0; i < 5; i++) {
		ptrs[i] = kmalloc(1024);
		L("#%d %p", i, ptrs[i]);
		kheap_node_set_id_at(ptrs[i], i);
		kheap_stats();
	}

	// 2. 释放中间的块，制造碎片
	L("free #1 %p", ptrs[1]);
	kfree(ptrs[1]);
	kheap_stats();

	for (int i = 0; i < 5; i++) {
		L("%d %p -> %p", i, ptrs[i] - sizeof(struct kheap_node), ptrs[i]);
	}

	L("free #3");
	kfree(ptrs[3]);
	kheap_stats(); // 应该看到断开的 FREE 块

	for (int i = 0; i < 5; i++) {
		L("%d %p -> %p", i, ptrs[i] - sizeof(struct kheap_node), ptrs[i]);
	}

	// 3. 释放相邻块，触发合并
	L("free #2");
	kfree(ptrs[2]);
	kheap_stats(); // 块 1， 2， 3 应该合并成一个大的 FREE 块

	L("vmallock(1024) for #3");
	ptrs[3] = kmalloc(1024);
	kheap_node_set_id_at(ptrs[3], 3);
	kheap_stats();

	L("free #0");
	kfree(ptrs[0]);
	kheap_stats();
	L("free #4");
	kfree(ptrs[4]);
	kheap_stats();
	L("free #3");
	kfree(ptrs[3]);

	kheap_stats();
}

void kheap_test()
{
	test_heap_recycle();
}
