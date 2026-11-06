/*
 * test_registry.h — 模块注册表自检
 *
 * test_registry_init() 将注册表自检注册为内置 selftest（"registry"）。
 * 由 task.conf 中的 @test registry 指令按需启用，不挂在正常启动路径上。
 *
 * 自检验证：
 *   1. reserve 可占满剩余空闲槽位
 *   2. registry full 时返回 -1
 *   3. cancel 正确释放槽位到 FREE
 *   4. 释放后的槽位可被后续 reserve 复用
 */
#ifndef __TEST_REGISTRY_H__
#define __TEST_REGISTRY_H__

void test_registry_init(void);

#endif
