#ifndef __TASK_H__
#define __TASK_H__

/*
 * task.h — 任务与进程管理接口
 */

#include "boot_info.h"
#include "boot_files.h"
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

	/* task.conf 中的模块位置参数。 */
	char args_buf[256];
	int argc;
	char **argv; /* 构造后交给线程持有，包含位置参数和 key=value。 */

	/* task.conf 中的 key=value 参数，由模块加载流程写入 TASK_DRIVER 参数变量。 */
	int kv_count;
	char kv_buf[256];
	char *kv_keys[16];
	char *kv_values[16];
};

enum task_conf_purpose {
	TASK_CONF_BASE,
	TASK_CONF_TEST,
	TASK_CONF_RECOVERY,
	TASK_CONF_COMPAT,
};

struct task_conf {
	struct list_node head;
	struct list_node directives;
	const struct boot_file_set *files;
	enum task_conf_purpose purpose;
	bool module_missing_panic;
	bool initialized;
	bool parsed;
	bool tests_started;
	uint64_t started_cpus;
	char config_path[BOOT_FILE_PATH_MAX];
};

int task_init(const struct boot_module_list *modules);
void task_run(void);
void task_run_cpu(const uint32_t cpu_id);
int task_run_configured_cpu_ctx(struct task_conf *conf, uint32_t cpu_id);
int task_init_ctx(struct task_conf *conf, const struct boot_file_set *files,
	enum task_conf_purpose purpose, const char *config_path);
void task_run_configured_cpu(const uint32_t cpu_id);

#endif
