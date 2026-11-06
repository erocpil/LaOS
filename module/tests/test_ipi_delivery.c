/*
 * test_ipi_delivery.c — IPI 投递压力自测载荷。
 *
 * 按配置轮数向在线 AP 发送 IPI，并逐轮确认每个 AP 恰好响应一次；每轮均设有
 * 独立超时，用于发现重复或遗漏投递。
 *
 * 由 task.conf 指令配置：
 *   @test ipi_delivery module=test_ipi_delivery.mo rounds=100 timeout_ticks=1000
 *
 * 使用专用的自测 IPI 向量，与 TLB shootdown 隔离，避免和其他测试或正常内核操作
 * 相互干扰。
 */
#include "tests/selftest.h"
#include "ipi.h"
#include "printf.h"
#include "cpu.h"
#include "export.h"
#include "string.h"

#define IPI_MAX_CPUS 64

/* 自测状态。 */
static volatile uint32_t s_rounds = 100;
static volatile uint32_t s_timeout_ticks = 1000;
static volatile uint32_t s_stage;
static volatile uint64_t s_expected_aps;
static volatile uint32_t s_current_round;
static volatile uint64_t s_elapsed;
static volatile uint64_t s_responses[IPI_MAX_CPUS];
static volatile bool s_done;
static volatile bool s_passed;

/* 在收到 IPI 的 AP 上运行的回调。 */
static void ipi_delivery_cb(void)
{
	/* 仅在实际投递阶段统计响应。 */
	if (__atomic_load_n(&s_stage, __ATOMIC_ACQUIRE) != 1) {
		return;
	}

	struct cpu_context *ctx = cpu_get_ctx();
	if (!ctx) {
		return;
	}
	unsigned int cpu = (unsigned int)ctx->id;
	if (cpu >= IPI_MAX_CPUS) {
		return;
	}

	/* 每次响应均递增计数，不做饱和处理；tick 路径通过每轮预期值 1 检测重复响应。 */
	module_atomic_add_fetch((volatile uint64_t *)&s_responses[cpu], 1);
}

/* 自测接口。 */
static void ipi_delivery_configure(const char *key, const char *value)
{
	uint32_t v = 0;
	for (const char *c = value; *c >= '0' && *c <= '9'; c++) {
		v = v * 10 + (uint32_t)(*c - '0');
	}

	if (strcmp(key, "rounds") == 0) {
		if (v == 0) {
			v = 100;
		}
		if (v > 10000) {
			v = 10000;
		}
		__atomic_store_n(&s_rounds, v, __ATOMIC_RELEASE);
	} else if (strcmp(key, "timeout_ticks") == 0) {
		if (v == 0) {
			v = 1000;
		}
		__atomic_store_n(&s_timeout_ticks, v, __ATOMIC_RELEASE);
	}
}

static void ipi_delivery_start(void)
{
	uint64_t total = __atomic_load_n(&online, __ATOMIC_ACQUIRE);
	s_expected_aps = (total > 0) ? total - 1 : 0;
	kprintf("[ipi_delivery] start: online=%llu expected=%llu "
		"rounds=%u timeout=%u ticks\n",
		total, s_expected_aps, s_rounds, s_timeout_ticks);
}

static void ipi_delivery_tick(void)
{
	if (__atomic_load_n(&s_done, __ATOMIC_ACQUIRE)) {
		return;
	}

	uint64_t expected = s_expected_aps;
	if (!expected) {
		kprintf("[ipi_delivery] passed: single CPU\n");
		__atomic_store_n(&s_passed, true, __ATOMIC_RELEASE);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		return;
	}

	uint32_t stage = __atomic_load_n(&s_stage, __ATOMIC_ACQUIRE);

	if (stage == 0) {
		/* 清零各 CPU 计数器并注册 IPI 回调。 */
		for (unsigned int i = 0; i < IPI_MAX_CPUS; i++) {
			__atomic_store_n(&s_responses[i], 0, __ATOMIC_RELEASE);
		}
		__atomic_store_n(&s_current_round, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&s_elapsed, 0, __ATOMIC_RELEASE);
		ipi_selftest_set_callback(ipi_delivery_cb);
		/* 先以 release 语义发布阶段状态，再广播 IPI，确保 AP 回调能观察到该状态。 */
		__atomic_store_n(&s_stage, 1, __ATOMIC_RELEASE);
		ipi_broadcast(IPI_VECTOR_SELFTEST);
		return;
	}

	/* 执行逐轮 IPI 投递。 */
	uint32_t current = __atomic_load_n(&s_current_round, __ATOMIC_ACQUIRE);
	uint32_t rounds = __atomic_load_n(&s_rounds, __ATOMIC_ACQUIRE);

	/* 检查本轮是否已收到所有 AP 的响应。 */
	unsigned int responded = 0;
	unsigned int duplicates = 0;

	for (unsigned int cpu = 0; cpu < IPI_MAX_CPUS; cpu++) {
		uint64_t cnt = __atomic_load_n(&s_responses[cpu], __ATOMIC_ACQUIRE);
		if (cnt > 1) {
			duplicates++;
		}
		if (cnt >= 1) {
			responded++;
		}
	}

	if (duplicates > 0) {
		kprintf("[ipi_delivery] FAIL: round %u — %u CPUs responded "
			"more than once\n",
			current, duplicates);
		__atomic_store_n(&s_passed, false, __ATOMIC_RELEASE);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		ipi_selftest_clear_callback(ipi_delivery_cb);
		return;
	}

	if (responded >= expected) {
		/* 本轮所有 AP 均恰好响应一次。 */
		if (current + 1 >= rounds) {
			kprintf("[ipi_delivery] passed: %u/%u rounds, "
				"%u APs per round\n",
				rounds, rounds, (unsigned int)expected);
			__atomic_store_n(&s_passed, true, __ATOMIC_RELEASE);
			__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
			ipi_selftest_clear_callback(ipi_delivery_cb);
			return;
		}
		/* 开始下一轮。 */
		__atomic_store_n(&s_current_round, current + 1, __ATOMIC_RELEASE);
		__atomic_store_n(&s_elapsed, 0, __ATOMIC_RELEASE);
		/* 清零各 CPU 计数器。 */
		for (unsigned int i = 0; i < IPI_MAX_CPUS; i++) {
			__atomic_store_n(&s_responses[i], 0, __ATOMIC_RELEASE);
		}
		ipi_selftest_set_callback(ipi_delivery_cb); /* 重新注册回调。 */
		ipi_broadcast(IPI_VECTOR_SELFTEST);
		return;
	}

	/* 尚有 AP 未响应，检查是否超时。 */
	uint64_t elapsed = ++s_elapsed;
	if (elapsed >= s_timeout_ticks) {
		kprintf("[ipi_delivery] FAIL: timeout at round %u — "
			"%u/%u APs responded\n",
			current, responded, (unsigned int)expected);
		__atomic_store_n(&s_passed, false, __ATOMIC_RELEASE);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		ipi_selftest_clear_callback(ipi_delivery_cb);
	}
}

static bool ipi_delivery_done(void)
{
	return __atomic_load_n(&s_done, __ATOMIC_ACQUIRE);
}

static bool ipi_delivery_passed(void)
{
	return __atomic_load_n(&s_passed, __ATOMIC_ACQUIRE);
}

static const struct selftest ipi_delivery_test = {
	.name = "ipi_delivery",
	.configure = ipi_delivery_configure,
	.start = ipi_delivery_start,
	.tick = ipi_delivery_tick,
	.done = ipi_delivery_done,
	.passed = ipi_delivery_passed,
};

/* 由内核自测载入器同步调用。 */
int selftest_init(void)
{
	kprintf("[ipi_delivery] selftest_init\n");
	return selftest_register(&ipi_delivery_test);
}
EXPORT_SYMBOL(selftest_init);
