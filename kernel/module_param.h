#ifndef __MODULE_PARAM_H__
#define __MODULE_PARAM_H__

/*
 * module_param.h — Linux 风格 key=value 模块参数系统。
 *
 * MODULE_PARAM(var, type, desc) 声明模块参数，生成 __laos_params 段条目。
 * 内核在加载模块后遍历 __laos_params 段，匹配 task.conf 的 key=value 对，
 * 直接写入模块的全局变量——模块 main() 执行时变量已是正确值。
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

#define MODULE_PARAM(_var, _type, _desc)                          \
	static struct laos_param __laos_param_##_var                  \
	__attribute__((used, section("__laos_params"))) = {          \
		.name = #_var,                                           \
		.type = PARAM_##_type,                                   \
		.ptr  = &(_var),                                         \
		.desc = _desc,                                           \
	}

#endif
