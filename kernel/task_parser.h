#ifndef __TASK_PARSER_H__
#define __TASK_PARSER_H__

/*
 * task.conf v1 解析接口。
 */

#include "boot_files.h"
struct task_conf;

int task_conf_parse_v1_ctx(struct task_conf *conf, const struct boot_file *file);

#endif
