/*
 * test_mutex.c - mutex 压力测试
 */

#include "mutex.h"
#include "test_mutex.h"
#include "cpu.h"
#include "sched.h"
#include "thread.h"
#include "printf.h"
#include "debug.h"

static int mutex_init_state;
struct mutex m;

void mutex_test_init(void)
{
	int expected = 0;
	if (__atomic_compare_exchange_n(
		    &mutex_init_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		mutex_init(&m);
		__atomic_store_n(&mutex_init_state, 2, __ATOMIC_RELEASE);
		return;
	}

	/* 其他 CPU 等待完整初始化发布后再创建或运行测试线程。 */
	while (__atomic_load_n(&mutex_init_state, __ATOMIC_ACQUIRE) != 2) {
		cpu_relax();
	}
}

volatile int loop = 0;

void func_mutex(void *data)
{
	int n = 0;
	uint32_t TIME = UINT32_MAX >> 1;
	struct mutex *m = (struct mutex *)data;

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
	char name[THREAD_NAME_MAX] = {0};
	ksprintf(name, "mutex-%d", id);
	thread_set_name(t, name);
	cpu_enqueue(id, t);
	L("%s %ld %d", t->name, t->id, t->target_cpu);
}
