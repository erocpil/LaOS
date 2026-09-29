/*
 * test_cpio.h — CPIO 解析器单元测试
 *
 * 两种使用方式：
 *   1. 直启路径：调用 test_cpio_run() 同步执行，直接输出 PASS/FAIL
 *   2. Limine 路径：test_cpio_init() 注册为 selftest（"cpio"），
 *      由 task.conf 中的 @test cpio 指令按需启用。
 *
 * 测试覆盖：
 *   1. cpio_init — 有效/无效/空/NULL 参数
 *   2. cpio_next — 迭代遍历 / TRAILER 终止
 *   3. cpio_find — 精确查找 / 未命中 / NULL 参数
 */
#ifndef __TEST_CPIO_H__
#define __TEST_CPIO_H__

#include <stdbool.h>

/** 注册为 selftest（供 task.conf @test 指令激活） */
void test_cpio_init(void);

/** 同步执行全部测试，返回 true 表示通过 */
bool test_cpio_run(void);

#endif
