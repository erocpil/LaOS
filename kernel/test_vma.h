/*
 * test_vma.h — VMA 数据结构单元测试
 *
 * 两种使用方式：
 *   1. 直启路径：调用 test_vma_run() 同步执行，直接输出 PASS/FAIL
 *   2. Limine 路径：test_vma_init() 注册为 selftest（"vma"），
 *      由 task.conf 中的 @test vma 指令按需启用。
 *
 * 测试覆盖：
 *   1. vma_alloc — 分配 + 排序插入验证
 *   2. vma_find — 命中/未命中/边界情况
 *   3. vma_free — 精确匹配释放 / 未匹配返回 -1
 *   4. vma_find_free — 间隙查找 / 空间不足
 *   5. vma_destroy_all — 全量清理
 */
#ifndef __TEST_VMA_H__
#define __TEST_VMA_H__

#include <stdbool.h>

/** 注册为 selftest（供 task.conf @test 指令激活） */
void test_vma_init(void);

/** 同步执行全部测试，返回 true 表示通过 */
bool test_vma_run(void);

#endif
