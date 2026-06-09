/*
 * 线程子系统冒烟测试入口。
 * 拆自 thread.c (R2 重构).
 */

#include "cpu.h"
#include "sched.h"
#include "heap.h"
#include "printf.h"
#include "thread.h"
#include "debug.h"
#include "arch_dispatch.h"
#include "thread_test.h"

#define N 1024
#define SLEEP_TIME 10000000

void task_func(void *data)
{
	int d = *(int*)data;
	int a = cpu_get_ctx()->id;
	int c = arch_cpu_apic_id();

	if (a != c || c != d) {
		panic("CPU ID");
	}

	int n = d * 100;
	while (n--) {
		if (d & 1) {
			schedule();
		} else {
			schedule_timeout(1);
		}
	}

	kfree(data);

	thread_exit(d);
}

void thread_test_common()
{
	int index = g_cpu_count;

	for (int i = 0; i < index; i++) {
		int *data = kmalloc(4);
		int a = i % index;
		int b = i / index;
		char name[16];
		ksprintf(name, "T-%d-%d", a, b);
		*data = a;
		struct thread *t = thread_create_on(task_func, (void*)data, a);
		if (t) {
			thread_set_name(t, name);
			cpu_enqueue(-1, t);
		} else {
			panic("Cannot create thread");
		}
	}
}
