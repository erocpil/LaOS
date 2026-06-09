/*
 * test_registry.c — 模块注册表三态自检
 *
 * 作为内置 selftest 注册，由 task.conf 中的 @test registry 指令
 * 按需启用。不挂在正常启动路径上——避免生产环境中临时占满 32 槽位。
 *
 * 自检序列（对已有模块透明，不假设注册表为空）：
 *   Phase 1: 逐槽位 reserve 直到失败 -> 记录已预留数 N
 *   Phase 2: 再 reserve 一次 -> 应返回 -1 (registry full)
 *   Phase 3: cancel 全部 N 个测试槽位 -> 回 FREE
 *   Phase 4: re-reserve 1 槽位 -> 应成功（复用已释放槽位）
 *   Phase 5: cancel 该槽位 -> 恢复原状
 *
 * 自检结束后注册表恢复至调用前状态。
 */

#include "test_registry.h"
#include "module.h"
#include "printf.h"
#include "string.h"
#include "selftest.h"

#define REGISTRY_SELFCHECK_MAX 32
#define REGISTRY_SELFCHECK_NAME "registry_test"

static int  registry_selfcheck_ids[REGISTRY_SELFCHECK_MAX];
static bool g_registry_test_ok;

static void registry_selftest_start(void)
{
	struct module_desc desc = {0};
	desc.kind = MODULE_KIND_SELFTEST;
	desc.name = REGISTRY_SELFCHECK_NAME;

	/* Phase 1: reserve until failure (accounts for pre-existing modules) */
	int n = 0;
	for (int i = 0; i < REGISTRY_SELFCHECK_MAX; i++) {
		int id = module_registry_reserve(&desc);
		if (id < 0) {
			break;
		}
		registry_selfcheck_ids[i] = id;
		n = i + 1;
	}

	if (n == 0) {
		kprintf("[registry_selfcheck] FAIL: cannot reserve any "
				"slot (all %d occupied)\n", REGISTRY_SELFCHECK_MAX);
		g_registry_test_ok = false;
		return;
	}
	kprintf("[registry_selfcheck] filled %d/%d free slots\n",
			n, REGISTRY_SELFCHECK_MAX);

	/* Phase 2: try to overflow — should fail */
	int overflow_id = module_registry_reserve(&desc);
	if (overflow_id >= 0) {
		kprintf("[registry_selfcheck] FAIL: overflow reserve "
				"should have failed (got id=%d)\n", overflow_id);
		module_registry_cancel((unsigned int)overflow_id);
		g_registry_test_ok = false;
		goto cleanup;
	}
	kprintf("[registry_selfcheck] overflow reserve correctly "
			"returned -1 (registry full)\n");

	/* Phase 3: cancel all test entries */
	for (int i = 0; i < n; i++) {
		module_registry_cancel((unsigned int)registry_selfcheck_ids[i]);
	}

	/* Phase 4: re-reserve one slot — should succeed */
	int re_reserved = module_registry_reserve(&desc);
	if (re_reserved < 0) {
		kprintf("[registry_selfcheck] FAIL: re-reserve after "
				"cancel should succeed\n");
		g_registry_test_ok = false;
		goto cleanup;
	}
	kprintf("[registry_selfcheck] re-reserve after cancel: id=%d\n",
			re_reserved);

	/* Phase 5: final cleanup */
	module_registry_cancel((unsigned int)re_reserved);

	size_t committed = module_registry_count();
	kprintf("[registry_selfcheck] PASSED (committed=%zu, "
			"filled=%d before full)\n", committed, n);
	g_registry_test_ok = true;

	return;

cleanup:
	for (int i = 0; i < n; i++) {
		module_registry_cancel((unsigned int)registry_selfcheck_ids[i]);
	}
}

static bool registry_selftest_done(void)
{
	return true;
}

static bool registry_selftest_passed(void)
{
	return g_registry_test_ok;
}

static const struct selftest registry_test = {
	.name = "registry",
	.start = registry_selftest_start,
	.done = registry_selftest_done,
	.passed = registry_selftest_passed,
};

void test_registry_init(void)
{
	if (selftest_register(&registry_test) < 0) {
		kprintf("[registry_selfcheck] WARNING: register failed\n");
	}
}
