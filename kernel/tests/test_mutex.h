#ifndef __TEST_MUTEX_H__
#define __TEST_MUTEX_H__

/*
 * test_mutex.h - mutex 测试声明
 */

extern struct mutex m;
extern volatile int loop;

void mutex_test_init(void);
void mutex_test_start_thread(int id);

#endif
