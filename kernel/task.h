#ifndef __TASK_H__
#define __TASK_H__

/*
 * task.h - 任务与进程管理接口
 */

#include "boot_info.h"
#include "list.h"

extern struct task_conf task_conf;

enum TASK_TYPE {
	TASK_KERNEL = 0,
	TASK_DRIVER,
	TASK_THREAD,
	TASK_USER,
	TASK_DATA,
	TASK_CONFIG,
	TASK_MAX,
};

struct task {
	struct list_node node;
	int cpu_id;
	int type;
	char info[64];
	char *module;
	char *name;
	uint64_t magic;
	void *elf;
	void *data;

	/* task.conf 第五列模块参数 */
	char args_buf[256];
	int argc;
	char *argv[16];

	/* task.conf kv 参数（key=value），内核在模块加载后直接写入模块变量 */
	int kv_count;
	char kv_buf[256];
	char *kv_keys[16];
	char *kv_values[16];
};

struct task_conf {
	struct list_node head;
	const struct boot_module_list *modules;
	bool module_missing_panic; /* @module_missing panic → crash on unknown module */
};

int task_init(const struct boot_module_list *modules);
void task_run(void);
void task_run_cpu(const uint32_t cpu_id);
void task_run_configured_cpu(const uint32_t cpu_id);

#endif
