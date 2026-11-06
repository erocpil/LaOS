/*
 * test_cpu_alive.c — 在线处理器响应能力自测载荷。
 *
 * 向所有在线 AP 发送一次 IPI，并确认每个 AP 均能接收和响应，用于快速检查
 * SMP 启动及 IPI 投递路径。
 *
 * 由 task.conf 指令配置：
 *   @test cpu_alive module=test_cpu_alive.mo timeout_ticks=100
 */
#include "tests/selftest.h"
#include "ipi.h"
#include "printf.h"
#include "cpu.h"
#include "export.h"
#include "string.h"

/* 自测状态。 */
static volatile uint32_t s_stage;
static volatile uint64_t s_expected_aps;
static volatile uint64_t s_ack_count;
static volatile uint32_t s_timeout_ticks = 100;
static volatile uint64_t s_elapsed;
static volatile bool s_done;
static volatile bool s_passed;

/* 在收到 IPI 的 AP 上运行的回调。 */
static void cpu_alive_ipi_cb(void)
{
	/* 仅在等待阶段统计响应。 */
	if (__atomic_load_n(&s_stage, __ATOMIC_ACQUIRE) != 1) {
		return;
	}
	module_atomic_add_fetch((volatile uint64_t *)&s_ack_count, 1);
}

/* 自测接口。 */
static void cpu_alive_configure(const char *key, const char *value)
{
	if (strcmp(key, "timeout_ticks") == 0) {
		uint32_t t = 0;
		for (const char *c = value; *c >= '0' && *c <= '9'; c++) {
			t = t * 10 + (uint32_t)(*c - '0');
		}
		if (t == 0) {
			t = 100;
		}
		__atomic_store_n(&s_timeout_ticks, t, __ATOMIC_RELEASE);
	}
}

static void cpu_alive_start(void)
{
	uint64_t total = __atomic_load_n(&online, __ATOMIC_ACQUIRE);
	s_expected_aps = (total > 0) ? total - 1 : 0;
	kprintf("[cpu_alive] start: online=%llu expected=%llu timeout=%u ticks\n", total,
		s_expected_aps, s_timeout_ticks);
}

static void cpu_alive_tick(void)
{
	if (__atomic_load_n(&s_done, __ATOMIC_ACQUIRE)) {
		return;
	}

	uint64_t expected = s_expected_aps;
	if (!expected) {
		kprintf("[cpu_alive] passed: single CPU (no APs to probe)\n");
		__atomic_store_n(&s_passed, true, __ATOMIC_RELEASE);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		return;
	}

	uint32_t stage = __atomic_load_n(&s_stage, __ATOMIC_ACQUIRE);

	if (stage == 0) {
		__atomic_store_n(&s_ack_count, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&s_elapsed, 0, __ATOMIC_RELEASE);
		ipi_selftest_set_callback(cpu_alive_ipi_cb);
		/* 先以 release 语义发布阶段状态，再广播 IPI，确保回调可观察到新状态。 */
		__atomic_store_n(&s_stage, 1, __ATOMIC_RELEASE);
		ipi_broadcast(IPI_VECTOR_SELFTEST);
		return;
	}

	/* 等待所有 AP 响应。 */
	uint64_t acked = __atomic_load_n(&s_ack_count, __ATOMIC_ACQUIRE);
	if (acked >= expected) {
		kprintf("[cpu_alive] passed: %llu/%llu APs responded\n", acked, expected);
		__atomic_store_n(&s_passed, true, __ATOMIC_RELEASE);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		ipi_selftest_clear_callback(cpu_alive_ipi_cb);
		return;
	}

	uint64_t elapsed = ++s_elapsed;
	if (elapsed >= s_timeout_ticks) {
		kprintf("[cpu_alive] FAIL: timeout (%llu ticks) — %llu/%llu APs responded\n",
			elapsed, acked, expected);
		__atomic_store_n(&s_passed, false, __ATOMIC_RELEASE);
		__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		ipi_selftest_clear_callback(cpu_alive_ipi_cb);
	}
}

static bool cpu_alive_done(void)
{
	return __atomic_load_n(&s_done, __ATOMIC_ACQUIRE);
}

static bool cpu_alive_passed(void)
{
	return __atomic_load_n(&s_passed, __ATOMIC_ACQUIRE);
}

static const struct selftest cpu_alive_test = {
	.name = "cpu_alive",
	.configure = cpu_alive_configure,
	.start = cpu_alive_start,
	.tick = cpu_alive_tick,
	.done = cpu_alive_done,
	.passed = cpu_alive_passed,
};

/* 由内核自测载入器同步调用。 */
int selftest_init(void)
{
	kprintf("[cpu_alive] selftest_init\n");
	return selftest_register(&cpu_alive_test);
}
EXPORT_SYMBOL(selftest_init);
