#ifndef __TEST_EXCEPTION_H__
#define __TEST_EXCEPTION_H__

#include "config.h"

/* 中断系统初始化完成后，按配置触发致命异常自测。 */
#if CONFIG_EXCEPTION_TEST
void test_exception_run(void);
#else
static inline void test_exception_run(void)
{
}
#endif

#endif
