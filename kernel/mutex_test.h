#ifndef __MUTEX_TEST_H__
#define __MUTEX_TEST_H__

/*
 * mutex_test.h - mutex 测试声明
 */

extern struct mutex m;
extern volatile int loop;

void mutex_test_init(void);
void mutex_test_start_thread(int id);

#endif
