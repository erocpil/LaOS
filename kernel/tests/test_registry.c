/*
 * test_registry.c — 模块注册表三态自检
 *
 * 作为内置 selftest 注册，由 task.conf 中的 @test registry 指令
 * 按需启用。不挂在正常启动路径上——避免生产环境中临时占满 32 槽位。
 *
 * 自检序列（对已有模块透明，不假设注册表为空）：
 *   阶段一：逐个预留槽位，直至失败，并记录成功预留的数量 N。
 *   阶段二：再次预留，确认注册表已满时返回 -1。
 *   阶段三：取消全部 N 个测试预留，使这些槽位恢复为空闲状态。
 *   阶段四：重新预留一个槽位，确认已释放的槽位可以复用。
 *   阶段五：取消该槽位，使注册表恢复至调用前状态。
 *
 * 自检结束后注册表恢复至调用前状态。
 */

#include "test_registry.h"
#include "module.h"
#include "printf.h"
#include "string.h"
#include "selftest.h"

#define REGISTRY_SELFCHECK_MAX	32
#define REGISTRY_SELFCHECK_NAME "registry_test"

static int registry_selfcheck_ids[REGISTRY_SELFCHECK_MAX];
static bool g_registry_test_ok;

static void registry_selftest_start(void)
{
	struct module_desc desc = {0};
	desc.kind = MODULE_KIND_SELFTEST;
	desc.name = REGISTRY_SELFCHECK_NAME;

	/* 逐个预留可用槽位；已有模块占用的槽位也计入注册表容量。 */
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
			"slot (all %d occupied)\n",
			REGISTRY_SELFCHECK_MAX);
		g_registry_test_ok = false;
		return;
	}
	kprintf("[registry_selfcheck] filled %d/%d free slots\n", n, REGISTRY_SELFCHECK_MAX);

	/* 注册表已满，再次预留应当失败。 */
	int overflow_id = module_registry_reserve(&desc);
	if (overflow_id >= 0) {
		kprintf("[registry_selfcheck] FAIL: overflow reserve "
			"should have failed (got id=%d)\n",
			overflow_id);
		module_registry_cancel((unsigned int)overflow_id);
		g_registry_test_ok = false;
		goto cleanup;
	}
	kprintf("[registry_selfcheck] overflow reserve correctly "
		"returned -1 (registry full)\n");

	/* 释放本测试预留的全部槽位。 */
	for (int i = 0; i < n; i++) {
		module_registry_cancel((unsigned int)registry_selfcheck_ids[i]);
	}

	/* 确认释放后的槽位可以重新预留。 */
	int re_reserved = module_registry_reserve(&desc);
	if (re_reserved < 0) {
		kprintf("[registry_selfcheck] FAIL: re-reserve after "
			"cancel should succeed\n");
		g_registry_test_ok = false;
		goto cleanup;
	}
	kprintf("[registry_selfcheck] re-reserve after cancel: id=%d\n", re_reserved);

	/* 释放最后重新预留的槽位。 */
	module_registry_cancel((unsigned int)re_reserved);

	size_t committed = module_registry_count();
	kprintf("[registry_selfcheck] PASSED (committed=%zu, "
		"filled=%d before full)\n",
		committed, n);
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
