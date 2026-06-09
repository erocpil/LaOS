/*
 * test_vma.c — VMA 链表操作单元测试
 *
 * 以虚拟线程为测试对象，验证 vma_alloc/find/free/find_free/destroy_all
 * 的完整语义。所有测试同步完成（start() 中全部执行）。
 */

#include "test_vma.h"
#include "vma.h"
#include "thread.h"
#include "heap.h"
#include "cpu.h"
#include "printf.h"
#include "string.h"
#include "selftest.h"

#define VMA_TEST_NAME "user_test"

static bool g_vma_test_ok;

/* 创建一个虚拟线程，vma_list 初始为空 */
static struct thread *make_dummy_thread(void)
{
	struct thread *t = kmalloc(sizeof(*t));
	if (!t) {
		return NULL;
	}

	memset(t, 0, sizeof(*t));
	memcpy(t->name, VMA_TEST_NAME, sizeof(VMA_TEST_NAME));

	return t;
}

/* 保存/恢复 current 线程，供 vma_find_free 测试用 */
static struct thread *swap_current(struct thread *new_cur)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *old = ctx->current;
	ctx->current = new_cur;

	return old;
}

static bool test_alloc_sorted(struct thread *t)
{
	/* 反向插入，验证排序 */
	struct vma *v3 = vma_alloc(t, 0x5000, 0x6000, PROT_READ, MAP_PRIVATE);
	struct vma *v1 = vma_alloc(t, 0x1000, 0x2000, PROT_READ, MAP_PRIVATE);
	struct vma *v2 = vma_alloc(t, 0x3000, 0x4000, PROT_WRITE, MAP_ANONYMOUS);

	if (!v1 || !v2 || !v3) {
		kprintf("[test_vma] FAIL: vma_alloc returned NULL\n");
		return false;
	}
	/* 验证排序：0x1000 → 0x3000 → 0x5000 */
	if (t->vma_list != v1 || v1->next != v2 || v2->next != v3 || v3->next != NULL) {
		kprintf("[test_vma] FAIL: sorted insertion broken\n");
		return false;
	}

	return true;
}

static bool test_find_hit(struct thread *t)
{
	/* t 已有 0x1000-0x2000, 0x3000-0x4000, 0x5000-0x6000 */

	/* 命中最前 */
	if (vma_find(t, 0x1000) == NULL) {
		kprintf("[test_vma] FAIL: find start-of-range miss\n");
		return false;
	}
	/* 命中中间 */
	if (vma_find(t, 0x3FFF) == NULL) {
		kprintf("[test_vma] FAIL: find end-of-range miss\n");
		return false;
	}
	/* 命中最后 */
	if (vma_find(t, 0x5000) == NULL) {
		kprintf("[test_vma] FAIL: find last VMA miss\n");
		return false;
	}

	return true;
}

static bool test_find_miss(struct thread *t)
{
	/* 间隙中 */
	if (vma_find(t, 0x2500) != NULL) {
		kprintf("[test_vma] FAIL: find gap should be miss\n");
		return false;
	}
	/* 末尾之后 */
	if (vma_find(t, 0x7000) != NULL) {
		kprintf("[test_vma] FAIL: find past-end should be miss\n");
		return false;
	}
	/* 开始之前 */
	if (vma_find(t, 0x500) != NULL) {
		kprintf("[test_vma] FAIL: find before-start should be miss\n");
		return false;
	}

	return true;
}

static bool test_free_exact(struct thread *t)
{
	/* vma_free 要求精确匹配 [start, start+len) */
	int rc = vma_free(t, 0x3000, 0x1000);
	if (rc != 0) {
		kprintf("[test_vma] FAIL: vma_free exact match failed (%d)\n", rc);
		return false;
	}
	/* 验证该 VMA 已移除 */
	if (vma_find(t, 0x3000) != NULL) {
		kprintf("[test_vma] FAIL: freed VMA still findable\n");
		return false;
	}
	/* 验证剩 2 个：0x1000 → 0x5000 */
	if (t->vma_list->start != 0x1000 || t->vma_list->next->start != 0x5000) {
		kprintf("[test_vma] FAIL: list corrupted after free\n");
		return false;
	}

	return true;
}

static bool test_free_mismatch(struct thread *t)
{
	/* 尺寸不对 */
	if (vma_free(t, 0x1000, 0x500) != -1) {
		kprintf("[test_vma] FAIL: partial-size free should return -1\n");
		return false;
	}
	/* 地址错 */
	if (vma_free(t, 0x9999, 0x1000) != -1) {
		kprintf("[test_vma] FAIL: nonexistent free should return -1\n");
		return false;
	}
	/* 列表无变化 */
	if (t->vma_list->start != 0x1000 || t->vma_list->next->start != 0x5000) {
		kprintf("[test_vma] FAIL: list changed after failed free\n");
		return false;
	}

	return true;
}

static bool test_find_free_gap(struct thread *t)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *old = swap_current(t);

	/* t 还有 0x1000-0x2000, 0x5000-0x6000
	 * 最小间隙应为 0x2000 后面（0x2000 → 0x5000 中间） */
	uint64_t addr = vma_find_free(0x500);
	ctx->current = old;

	if (addr == 0) {
		kprintf("[test_vma] FAIL: vma_find_free should find gap\n");
		return false;
	}
	/* 应在 0x2000..0x4FFF 范围内 */
	if (addr < 0x2000 || addr > 0x4FFF) {
		kprintf("[test_vma] FAIL: vma_find_free unexpected address 0x%lx\n", addr);
		return false;
	}

	return true;
}

static bool test_find_free_nospace(struct thread *t)
{
	/* 清空前面的测试 VMA，确保从 USER_MMAP_BASE 开始填充无空隙 */
	vma_destroy_all(t);

	/* 填满 0x4000000 → 0x10000000 之间的空间 */
	uint64_t filler = 0x4000000;
	while (filler < 0x10000000) {
		vma_alloc(t, filler, filler + 0x100000, PROT_READ, MAP_ANONYMOUS);
		filler += 0x100000;
	}

	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *old = swap_current(t);
	uint64_t addr = vma_find_free(0x1000);  /* 超过末尾，不应有空间 */
	ctx->current = old;

	if (addr != 0) {
		kprintf("[test_vma] FAIL: vma_find_free should return 0 on full space, got 0x%lx\n", addr);
		return false;
	}

	return true;
}

static void test_vma_start(void)
{
	g_vma_test_ok = test_vma_run() ? true : false;
}

static bool test_vma_done(void) { return true; }
static bool test_vma_passed(void) { return g_vma_test_ok; }

static const struct selftest vma_test = {
	.name = "vma",
	.start = test_vma_start,
	.done = test_vma_done,
	.passed = test_vma_passed,
};

void test_vma_init(void)
{
	if (selftest_register(&vma_test) < 0) {
		kprintf("[test_vma] WARNING: register failed\n");
	} else {
		kprintf("[test_vma] registered\n");
	}
}

/**
 * test_vma_run() — 同步执行完整 VMA 单元测试
 *
 * 直启路径调用此函数，可在线程/调度器启动前验证
 * VMA 数据结构的正确性。返回 true 表示全部 7 项测试通过。
 */
bool test_vma_run(void)
{
	bool ok = true;

	struct thread *t = make_dummy_thread();
	if (!t) {
		kprintf("[test_vma] FAIL: cannot allocate dummy thread\n");
		return false;
	}

	kprintf("[test_vma] 1/7 vma_alloc sorted insertion...");
	if (!test_alloc_sorted(t)) {
		ok = false;
		goto out;
	}
	kprintf(" ok\n");

	kprintf("[test_vma] 2/7 vma_find hit...");
	if (!test_find_hit(t)) {
		ok = false;
		goto out;
	}
	kprintf(" ok\n");

	kprintf("[test_vma] 3/7 vma_find miss...");
	if (!test_find_miss(t)) {
		ok = false;
		goto out;
	}
	kprintf(" ok\n");

	kprintf("[test_vma] 4/7 vma_free exact...");
	if (!test_free_exact(t)) {
		ok = false;
		goto out;
	}
	kprintf(" ok\n");

	kprintf("[test_vma] 5/7 vma_free mismatch...");
	if (!test_free_mismatch(t)) {
		ok = false;
		goto out;
	}
	kprintf(" ok\n");

	kprintf("[test_vma] 6/7 vma_find_free gap...");
	if (!test_find_free_gap(t)) {
		ok = false;
		goto out;
	}
	kprintf(" ok\n");

	kprintf("[test_vma] 7/7 vma_find_free nospace...");
	if (!test_find_free_nospace(t)) {
		ok = false;
		goto out;
	}
	kprintf(" ok\n");

out:
	vma_destroy_all(t);
	kfree(t);
	kprintf("[test_vma] %s\n", ok ? "PASSED" : "FAILED");

	return ok;
}
