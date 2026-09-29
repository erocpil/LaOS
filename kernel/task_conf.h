#ifndef __TASK_CONF_H__
#define __TASK_CONF_H__

/*
 * task_conf.h - task.conf boot-time orchestration DSL state and helpers.
 *
 * task.conf parsing is a core boot facility and remains statically linked.
 * Directive records (@test, @set, ...) are produced by the parser and
 * consumed later by selftest / other subsystems — the parser itself
 * has no side effects beyond filling these records.
 */

#include <stdbool.h>
#include "boot_info.h"
#include "list.h"
#include "selftest.h"

/* Global task.conf state */
extern struct task_conf task_conf;

void task_conf_init(const struct boot_module_list *modules);
int task_conf_parse_module(const struct boot_module *module);
bool task_conf_has_ap_tasks(void);

/* Directive management — parser produces, consumers read */
void task_conf_add_directive(const struct selftest_directive *d);
bool task_conf_has_directives(void);
struct list_node *task_conf_get_directives(void);

#endif
