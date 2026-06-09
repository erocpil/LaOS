/*
 * selftest.h - 内核自测试子系统
 *
 * 测试通过 selftest_register() 注册自身，通过 selftest_apply_all() 消费
 * task.conf @test directive 产生的配置记录，由主循环 selftest_tick() 驱动。
 *
 * 设计边界：
 *   - task_parser 只产出 directive records（纯解析，零副作用）
 *   - selftest.c 统一消费 records，完成配置和调度
 *   - 加新测试类型只需注册新的 selftest，不改 parser
 */

#ifndef __SELFTEST_H__
#define __SELFTEST_H__

#include <stdint.h>
#include <stdbool.h>
#include "list.h"

/* 单个 key=value 配置项 */
struct selftest_kv {
	char key[32];
	char value[32];
};

/* 一个 directive record：@test <name> [key=value [...]] */
#define SELFTEST_MAX_KV 8

struct selftest_directive {
	char name[32];
	struct selftest_kv kvs[SELFTEST_MAX_KV];
	int kv_count;
	int module_id;  /* registry ID, set by selftest_load_payloads() */
	struct list_node node;
};

/* 一个已注册的测试 */
struct selftest {
	const char *name;

	/* configure: 逐 kv 对配置（可多次调用，在 start 之前） */
	void (*configure)(const char *key, const char *value);

	/* prepare: configuration完成后在普通线程上下文调用。用于预分配
	 * worker 等不能在 timer ISR 中执行的资源。0=成功，负值=失败。 */
	int (*prepare)(void);

	/* start: 所有 CPU online 后调用一次 */
	void (*start)(void);

	/* tick: 调度主循环中每 tick 调用一次 */
	void (*tick)(void);

	/* done: 返回 true 表示测试完成（测试完成后不再调 tick） */
	bool (*done)(void);

	/* passed: 返回 true 表示测试通过。仅当 done()==true 时调用。
	 * NULL 等价于返回 true（无显式结果 = 通过）。 */
	bool (*passed)(void);
};

/* 注册一个测试。应在 INIT 阶段（编译期链接或早期 initcall）调用。 */
int selftest_register(const struct selftest *test);

/* 从 directive records 链表消费所有配置。
 * 每遇到一个 @test name=xxx，查找注册表中同名测试并调 configure()。 */
void selftest_apply_all(struct list_node *directives);

/* 遍历 directives，加载 module= 指定的测试载荷 .mo 模块。
 * 载荷的 selftest_init() 在此阶段被同步调用（selftest_register）。
 * 应在 task_init() 之后、selftest_apply_all() 之前调用。 */
#include "boot_info.h"
void selftest_load_payloads(const struct boot_module_list *modules,
			    struct list_node *directives);

/* 启动所有已配置的测试（调 start），通常在 AP online 之后调用。 */
void selftest_run(void);

/* 驱动所有运行中的测试（调 tick）。在调度主循环中调用。 */
void selftest_tick(void);

/* 所有已配置的测试是否都已完成 */
bool selftest_all_done(void);

/* 两阶段 worker API：create/discard 在 process-context prepare() 中使用，
 * start 仅入队，可安全用于稍后的 selftest start 阶段。 */
#include "thread.h"
struct thread *selftest_create_worker(int cpu, void (*entry)(void*),
				     const char *name, void *data);
void selftest_start_worker(struct thread *t);
void selftest_discard_worker(struct thread *t);

/* Atomic add-fetch wrapper for module use.
 * Modules cannot link against libgcc, which ARM64 requires for
 * __atomic_add_fetch (lowered to __aarch64_ldadd8_rel).  The kernel
 * exports this wrapper so modules can use it without libgcc linkage. */
uint64_t module_atomic_add_fetch(volatile uint64_t *ptr, uint64_t val);

#endif
