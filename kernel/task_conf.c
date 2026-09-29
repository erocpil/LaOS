/*
 * task_conf.c - task.conf boot-time orchestration DSL state.
 */
#include "task_conf.h"
#include "task.h"
#include "task_parser.h"
#include "list.h"
#include "heap.h"
#include "string.h"

struct task_conf task_conf;
static struct list_node task_conf_directives;
static bool task_conf_directives_init;

void task_conf_init(const struct boot_module_list *modules)
{
	list_init(&task_conf.head);
	task_conf.modules = modules;
	if (!task_conf_directives_init) {
		list_init(&task_conf_directives);
		task_conf_directives_init = true;
	}
}

int task_conf_parse_module(const struct boot_module *module)
{
	return task_conf_parse_legacy_v1(module);
}

bool task_conf_has_ap_tasks(void)
{
	if (!task_conf.head.next) {
		return false;
	}

	struct task *task;
	list_for_each_entry(task, &task_conf.head, node) {
		if (task->cpu_id > 0)
			return true;
	}

	return false;
}

void task_conf_add_directive(const struct selftest_directive *d)
{
	struct selftest_directive *copy = kmalloc(sizeof(*copy));
	if (!copy) {
		return;
	}
	memcpy(copy, d, sizeof(*copy));
	list_add_tail(&copy->node, &task_conf_directives);
}

bool task_conf_has_directives(void)
{
	return !list_empty(&task_conf_directives);
}

struct list_node *task_conf_get_directives(void)
{
	return &task_conf_directives;
}
