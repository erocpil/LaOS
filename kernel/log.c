/*
 * log.c - 日志模块标签注册与格式化输出
 */

#include "log.h"
#include "string.h"
#include "export.h"

static int log_initialized = 0;
int g_log_tag_width = 0;

void log_init(void)
{
	if (log_initialized) {
		return;
	}

	for (int i = 0; i < LOG_MOD_COUNT; i++) {
		int len = strlen(g_log_modules[i].name);
		if (len > g_log_tag_width) {
			g_log_tag_width = len;
		}
	}

	log_initialized = 1;
}

inline int log_get_tag_width(void)
{
	return g_log_tag_width;
}
EXPORT_SYMBOL(log_get_tag_width);
