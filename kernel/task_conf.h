#ifndef __TASK_CONF_H__
#define __TASK_CONF_H__

#include "task.h"
#include "tests/selftest.h"

/* 显式上下文须先清零，再初始化并解析。配置源及载荷均为借用数据；已启动的
 * 上下文不可销毁。 */
int task_conf_init_ctx(
	struct task_conf *conf, const struct boot_file_set *files, enum task_conf_purpose purpose);
int task_conf_parse_ctx(struct task_conf *conf, const struct boot_file *file);
int task_conf_load_ctx(struct task_conf *conf, const char *path);
/* CPU 启动认领由调用方串行化；任务启动器使用 task_lock。 */
int task_conf_claim_cpu_ctx(struct task_conf *conf, uint32_t cpu_id);
int task_conf_destroy_ctx(struct task_conf *conf);
bool task_conf_has_ap_tasks_ctx(const struct task_conf *conf);
int task_conf_add_directive_ctx(struct task_conf *conf, const struct selftest_directive *d);
bool task_conf_has_directives_ctx(const struct task_conf *conf);

/* 供现有架构入口使用的默认上下文适配接口。 */
extern struct task_conf task_conf;
void task_conf_init(const struct boot_module_list *modules);
int task_conf_parse_module(const struct boot_module *module);
bool task_conf_has_ap_tasks(void);
bool task_conf_has_directives(void);
struct list_node *task_conf_get_directives(void);
#endif
