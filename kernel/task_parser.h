#ifndef __TASK_PARSER_H__
#define __TASK_PARSER_H__

/*
 * task_parser.h - task.conf 解析接口
 */

#include "boot_info.h"

int task_conf_parse_legacy_v1(const struct boot_module *f);

#endif
