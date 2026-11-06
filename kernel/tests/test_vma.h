/*
 * test_vma.h — VMA 数据结构单元测试
 *
 * 启动阶段由 runner 注册并调用 test_vma_run()，在真实设备注册前执行。
 * task.conf 中的 @test vma 只报告该次测试结果，不重新执行 fixture。
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

/* 注册为 selftest（供 task.conf @test 指令激活） */
void test_vma_init(void);

/* 仅在 MEMORY_READY 阶段同步执行全部测试，返回 true 表示通过 */
bool test_vma_run(void);

#endif
