#ifndef __MODULE_PARAM_H__
#define __MODULE_PARAM_H__

/*
 * module_param.h — 模块 key=value 参数接口。
 *
 * MODULE_PARAM(var, type, desc) 声明模块参数，生成 __laos_params 段条目。
 * 内核在加载模块后遍历 __laos_params 段，匹配 task.conf 的 key=value 对，
 * TASK_DRIVER 创建时写入模块全局变量；线程实例配置应由入口解析 argv。
 * TASK_THREAD 不自动应用本机制；其 key=value 通过 argc/argv 传递。
 */

enum laos_param_type {
	PARAM_INT = 0,
	PARAM_STRING,
	PARAM_BOOL,
};

struct laos_param {
	const char *name;
	int type;
	void *ptr;
	const char *desc;
};

#define MODULE_PARAM(_var, _type, _desc)                                                           \
	static struct laos_param __laos_param_##_var                                               \
		__attribute__((used, section("__laos_params"))) = {                                \
			.name = #_var,                                                             \
			.type = PARAM_##_type,                                                     \
			.ptr = &(_var),                                                            \
			.desc = _desc,                                                             \
	}

#endif
