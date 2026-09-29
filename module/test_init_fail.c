/*
 * test_init_fail.c — selftest_init() 返回 -1 的测试载荷。
 *
 * 用于验证 registry RESERVED → cancel 的失败路径：
 * selftest_load_payload() 在 init 返回负值时取消预留并回滚分配。
 * 该模块不应出现在 registry dump 中。
 */
#include "selftest.h"
#include "export.h"

int selftest_init(void)
{
	return -1;
}
EXPORT_SYMBOL(selftest_init);
