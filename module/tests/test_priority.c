/*
 * test_priority.c — 固定优先级运行队列与互斥锁优先级继承自测。
 *
 * 第一阶段验证优先级选择及同优先级线程的 FIFO 顺序。第二阶段在单个 CPU 上构造
 * 优先级反转：低优先级线程持有互斥锁，高优先级线程等待该锁，中优先级线程可运行；
 * 优先级继承应在解锁前提升低优先级线程，使其优先于中优先级线程运行。
 */

#include "tests/selftest.h"
#include "export.h"
#include "thread.h"
#include "mutex.h"
#include "cpu.h"
#include "sched.h"
#include "printf.h"
#include "string.h"

#define PRIO_HIGH   8
#define PRIO_MEDIUM 32
#define PRIO_LOW    48

enum priority_phase {
	PRIORITY_ORDERING,
	PRIORITY_INVERSION,
	PRIORITY_FINISHED,
};

struct order_worker_arg {
	volatile uint64_t *rank;
};

static struct mutex pi_mutex;
static struct thread *controller;
static struct thread *order_high;
static struct thread *order_equal_a;
static struct thread *order_equal_b;
static struct thread *order_low;
static struct thread *pi_low;
static struct thread *pi_medium;
static struct thread *pi_high;

static struct order_worker_arg order_args[4];
static volatile uint64_t order_sequence;
static volatile uint64_t order_rank_high;
static volatile uint64_t order_rank_equal_a;
static volatile uint64_t order_rank_equal_b;
static volatile uint64_t order_rank_low;
static volatile bool order_go;
static volatile bool order_low_promoted;

static volatile bool pi_go;
static volatile bool pi_high_attempted;
static volatile bool pi_low_saw_boost;
static volatile bool pi_low_finished;
static volatile bool pi_high_finished;
static volatile bool pi_medium_ran;
static volatile bool pi_medium_ran_before_unlock;
static volatile int pi_low_priority_after_unlock;

static volatile uint64_t elapsed_ticks;
static uint64_t timeout_ticks = 500;
static volatile bool failed;
static enum priority_phase phase;
static int target_cpu;

static uint32_t parse_u32(const char *value, uint32_t fallback)
{
	uint32_t result = 0;

	if (!value || *value == '\0') {
		return fallback;
	}
	for (const char *p = value; *p >= '0' && *p <= '9'; p++) {
		result = result * 10U + (uint32_t)(*p - '0');
	}

	return result ? result : fallback;
}

static void priority_configure(const char *key, const char *value)
{
	if (strcmp(key, "timeout_ticks") == 0) {
		timeout_ticks = parse_u32(value, 500);
	}
}

static void order_worker(void *arg)
{
	struct order_worker_arg *worker = arg;

	while (!__atomic_load_n(&order_go, __ATOMIC_ACQUIRE)) {
		schedule();
	}

	uint64_t rank = module_atomic_add_fetch(&order_sequence, 1);
	__atomic_store_n(worker->rank, rank, __ATOMIC_RELEASE);
	kprintf("[priority] order worker rank=%llu cpu=%d\n", rank, cpu_get_ctx()->id);
}

static void priority_controller(void *arg)
{
	(void)arg;
	kprintf("[priority] controller running cpu=%d\n", cpu_get_ctx()->id);

	selftest_start_worker(order_low);
	selftest_start_worker(order_equal_a);
	selftest_start_worker(order_equal_b);
	selftest_start_worker(order_high);
	__atomic_store_n(&order_go, true, __ATOMIC_RELEASE);
}

static void pi_low_worker(void *arg)
{
	(void)arg;

	mutex_lock(&pi_mutex);
	/* 先以较高优先级完成准备；反转场景建立后，再调整为预期的低优先级锁持有者。 */
	if (thread_set_priority(get_current(), PRIO_LOW) < 0) {
		__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
	}
	selftest_start_worker(pi_medium);
	selftest_start_worker(pi_high);
	__atomic_store_n(&pi_go, true, __ATOMIC_RELEASE);

	while (!__atomic_load_n(&pi_high_attempted, __ATOMIC_ACQUIRE)) {
		schedule();
	}

	if (get_current()->priority == PRIO_HIGH) {
		__atomic_store_n(&pi_low_saw_boost, true, __ATOMIC_RELEASE);
	}
	if (__atomic_load_n(&pi_medium_ran, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&pi_medium_ran_before_unlock, true, __ATOMIC_RELEASE);
	}

	mutex_unlock(&pi_mutex);
	__atomic_store_n(&pi_low_priority_after_unlock, get_current()->priority, __ATOMIC_RELEASE);
	__atomic_store_n(&pi_low_finished, true, __ATOMIC_RELEASE);
}

static void pi_high_worker(void *arg)
{
	(void)arg;

	while (!__atomic_load_n(&pi_go, __ATOMIC_ACQUIRE)) {
		schedule();
	}
	__atomic_store_n(&pi_high_attempted, true, __ATOMIC_RELEASE);
	mutex_lock(&pi_mutex);
	mutex_unlock(&pi_mutex);
	__atomic_store_n(&pi_high_finished, true, __ATOMIC_RELEASE);
}

static void pi_medium_worker(void *arg)
{
	(void)arg;

	while (!__atomic_load_n(&pi_go, __ATOMIC_ACQUIRE)) {
		schedule();
	}
	__atomic_store_n(&pi_medium_ran, true, __ATOMIC_RELEASE);
}

static void discard_priority_workers(void)
{
	struct thread *workers[] = {controller, order_high, order_equal_a, order_equal_b, order_low,
		pi_low, pi_medium, pi_high};
	for (unsigned i = 0; i < sizeof(workers) / sizeof(workers[0]); i++) {
		selftest_discard_worker(workers[i]);
	}
	controller = order_high = order_equal_a = order_equal_b = order_low = NULL;
	pi_low = pi_medium = pi_high = NULL;
}

static int priority_prepare(void)
{
	if (!g_cpu_contexts[target_cpu]) {
		kprintf("[priority] FAIL: target CPU is not online\n");
		return -1;
	}

	order_sequence = 0;
	order_rank_high = 0;
	order_rank_equal_a = 0;
	order_rank_equal_b = 0;
	order_rank_low = 0;
	order_go = false;
	order_low_promoted = false;
	pi_go = false;
	pi_high_attempted = false;
	pi_low_saw_boost = false;
	pi_low_finished = false;
	pi_high_finished = false;
	pi_medium_ran = false;
	pi_medium_ran_before_unlock = false;
	pi_low_priority_after_unlock = -1;
	elapsed_ticks = 0;
	failed = false;
	phase = PRIORITY_ORDERING;
	mutex_init(&pi_mutex);

	order_args[0].rank = &order_rank_high;
	order_args[1].rank = &order_rank_equal_a;
	order_args[2].rank = &order_rank_equal_b;
	order_args[3].rank = &order_rank_low;
	controller = selftest_create_worker(target_cpu, priority_controller, "prio-control", NULL);
	order_high = selftest_create_worker(target_cpu, order_worker, "prio-high", &order_args[0]);
	order_equal_a =
		selftest_create_worker(target_cpu, order_worker, "prio-equal-a", &order_args[1]);
	order_equal_b =
		selftest_create_worker(target_cpu, order_worker, "prio-equal-b", &order_args[2]);
	order_low = selftest_create_worker(target_cpu, order_worker, "prio-low", &order_args[3]);
	pi_low = selftest_create_worker(target_cpu, pi_low_worker, "pi-low", NULL);
	pi_medium = selftest_create_worker(target_cpu, pi_medium_worker, "pi-medium", NULL);
	pi_high = selftest_create_worker(target_cpu, pi_high_worker, "pi-high", NULL);
	if (!controller || !order_high || !order_equal_a || !order_equal_b || !order_low ||
		!pi_low || !pi_medium || !pi_high) {
		discard_priority_workers();
		return -1;
	}

	if (thread_set_priority(controller, PRIO_HIGH) < 0 ||
		thread_set_priority(order_high, PRIO_HIGH) < 0 ||
		thread_set_priority(order_equal_a, SCHED_DEFAULT_PRIO) < 0 ||
		thread_set_priority(order_equal_b, SCHED_DEFAULT_PRIO) < 0 ||
		thread_set_priority(order_low, PRIO_LOW) < 0 ||
		thread_set_priority(pi_low, PRIO_HIGH) < 0 ||
		thread_set_priority(pi_medium, PRIO_MEDIUM) < 0 ||
		thread_set_priority(pi_high, PRIO_HIGH) < 0 ||
		thread_set_priority(order_high, -1) != -1 ||
		thread_set_priority(order_high, SCHED_PRIO_COUNT) != -1) {
		kprintf("[priority] FAIL: priority API validation\n");
		discard_priority_workers();
		return -1;
	}

	return 0;
}

static void priority_start(void)
{
	kprintf("[priority] start current=%s prio=%u cpu=%d\n", get_current()->name,
		get_current()->priority, cpu_get_ctx()->id);
	/* 一次远程唤醒启动控制线程；其他测试线程随后由控制线程加入目标 CPU 的队列，
	 * 避免重复测试 remote_enqueue 的 IPI 时序。 */
	selftest_start_worker(controller);
}

static void priority_tick(void)
{
	if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) {
		return;
	}

	uint64_t elapsed = module_atomic_add_fetch(&elapsed_ticks, 1);
	if (elapsed >= timeout_ticks) {
		kprintf("[priority] FAIL: timeout in phase %d\n", phase);
		__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
		return;
	}

	if (phase == PRIORITY_ORDERING && __atomic_load_n(&order_rank_equal_b, __ATOMIC_ACQUIRE) &&
		!__atomic_load_n(&order_rank_low, __ATOMIC_ACQUIRE) &&
		!__atomic_load_n(&order_low_promoted, __ATOMIC_RELAXED)) {
		/* 高优先级队列非空时，低优先级线程不应运行。待其他线程完成后再提升其优先级，
		 * 同时验证已入队节点的优先级迁移。 */
		if (thread_set_priority(order_low, PRIO_HIGH) < 0) {
			__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
			return;
		}
		__atomic_store_n(&order_low_promoted, true, __ATOMIC_RELEASE);
		return;
	}

	if (phase == PRIORITY_ORDERING && __atomic_load_n(&order_rank_low, __ATOMIC_ACQUIRE)) {
		bool ordered = order_rank_high == 1 && order_rank_equal_a == 2 &&
			       order_rank_equal_b == 3 && order_rank_low == 4;
		kprintf("[priority] order high=%llu equal-a=%llu "
			"equal-b=%llu low=%llu\n",
			order_rank_high, order_rank_equal_a, order_rank_equal_b, order_rank_low);
		if (!ordered) {
			__atomic_store_n(&failed, true, __ATOMIC_RELEASE);
			return;
		}

		phase = PRIORITY_INVERSION;
		selftest_start_worker(pi_low);
		return;
	}

	if (phase == PRIORITY_INVERSION && __atomic_load_n(&pi_low_finished, __ATOMIC_ACQUIRE) &&
		__atomic_load_n(&pi_high_finished, __ATOMIC_ACQUIRE) &&
		__atomic_load_n(&pi_medium_ran, __ATOMIC_ACQUIRE)) {
		phase = PRIORITY_FINISHED;
	}
}

static bool priority_done(void)
{
	return __atomic_load_n(&failed, __ATOMIC_ACQUIRE) || phase == PRIORITY_FINISHED;
}

static bool priority_passed(void)
{
	bool ok = !__atomic_load_n(&failed, __ATOMIC_ACQUIRE) && phase == PRIORITY_FINISHED &&
		  __atomic_load_n(&pi_low_saw_boost, __ATOMIC_ACQUIRE) &&
		  !__atomic_load_n(&pi_medium_ran_before_unlock, __ATOMIC_ACQUIRE) &&
		  pi_low_priority_after_unlock == PRIO_LOW;

	kprintf("[priority] %s: boost=%d medium_before_unlock=%d "
		"restored=%d\n",
		ok ? "PASSED" : "FAILED", pi_low_saw_boost, pi_medium_ran_before_unlock,
		pi_low_priority_after_unlock);
	return ok;
}

static const struct selftest priority_test = {
	.name = "priority",
	.configure = priority_configure,
	.prepare = priority_prepare,
	.start = priority_start,
	.tick = priority_tick,
	.done = priority_done,
	.passed = priority_passed,
};

int selftest_init(void)
{
	return selftest_register(&priority_test);
}
EXPORT_SYMBOL(selftest_init);
