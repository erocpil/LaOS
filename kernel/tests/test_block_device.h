/*
 * test_block_device.h — block_device 单元测试
 */
#ifndef __TEST_BLOCK_DEVICE_H__
#define __TEST_BLOCK_DEVICE_H__

#include <stdbool.h>

/* 注册缓存结果；运行期 @test 不重新执行 fixture。 */
void test_block_device_init(void);
/* 仅在 MEMORY_READY 阶段、真实设备注册前执行。 */
bool test_block_device_run(void);

#endif /* __TEST_BLOCK_DEVICE_H__ */
