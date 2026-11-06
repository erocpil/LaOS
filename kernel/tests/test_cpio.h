/*
 * test_cpio.h — CPIO 解析器单元测试
 *
 * 启动阶段由 runner 注册并调用 test_cpio_run()，在真实设备注册前执行。
 * task.conf 中的 @test cpio 只报告该次测试结果，不重新执行 fixture。
 *
 * 测试覆盖：
 *   1. cpio_init — 有效/无效/空/NULL 参数
 *   2. cpio_next — 迭代遍历 / TRAILER 终止
 *   3. cpio_find — 精确查找 / 未命中 / NULL 参数
 */
#ifndef __TEST_CPIO_H__
#define __TEST_CPIO_H__

#include <stdbool.h>

/* 注册为 selftest（供 task.conf @test 指令激活） */
void test_cpio_init(void);

/* 仅在 MEMORY_READY 阶段同步执行全部测试，返回 true 表示通过 */
bool test_cpio_run(void);

#endif
