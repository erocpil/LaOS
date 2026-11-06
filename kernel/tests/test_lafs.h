/*
 * test_lafs.h — LaFS 文件系统单元测试
 */
#ifndef __TEST_LAFS_H__
#define __TEST_LAFS_H__

#include <stdbool.h>

/* 注册缓存结果；运行期 @test 不重新执行 fixture。 */
void test_lafs_init(void);
/* 仅在 MEMORY_READY 阶段、真实设备注册前执行。 */
bool test_lafs_run(void);

#endif /* __TEST_LAFS_H__ */
