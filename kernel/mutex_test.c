/*
 * mutex_test.c - mutex 压力测试
 */

#include "mutex.h"
#include "mutex_test.h"
#include "cpu.h"
#include "sched.h"
#include "thread.h"
#include "printf.h"
#include "debug.h"

static volatile int mutex_inited = 0;
struct mutex m;

void mutex_test_init(void)
{
	/* __sync CAS 保证 AP/BSP 并发安全：只有一条路径进入初始化 */
	if (__sync_bool_compare_and_swap(&mutex_inited, 0, 1)) {
		mutex_init(&m);
	}
}

volatile int loop = 0;

void func_mutex(void *data)
{
	int n = 0;
	uint32_t TIME = UINT32_MAX >> 1;
	struct mutex *m = (struct mutex*)data;

	for (uint32_t i = 0; i < TIME; i++) {
		mutex_lock(m);
		loop++;
		mutex_unlock(m);
		if (n++ & 1) {
			schedule_timeout(1);
		} else {
			schedule();
		}
	}

	thread_exit(n);
}

void mutex_test_start_thread(int id)
{
	mutex_test_init();

	struct thread *t = thread_create_on(func_mutex, &m, id);
	char name[THREAD_NAME_MAX] = { 0 };
	ksprintf(name, "mutex-%d", id);
	thread_set_name(t, name);
	cpu_enqueue(id, t);
	L("%s %ld %d", t->name, t->id, t->target_cpu);
}
