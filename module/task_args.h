#ifndef LAOS_MODULE_TASK_ARGS_H
#define LAOS_MODULE_TASK_ARGS_H

/* 解析线程入口的 argv 参数；参数存储仍由内核线程持有，模块不得释放。 */
static inline int task_arg_equal(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static inline const char *task_arg_value(const char *arg, const char *key)
{
	while (*key && *arg == *key) {
		arg++;
		key++;
	}
	return !*key && *arg == '=' ? arg + 1 : 0;
}

static inline int task_arg_uint(const char *s, int min, int max, int *out)
{
	if (!*s) {
		return -1;
	}
	unsigned value = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9') {
			return -1;
		}
		unsigned digit = (unsigned)(*s - '0');
		if (digit > (unsigned)max || value > ((unsigned)max - digit) / 10) {
			return -1;
		}
		value = value * 10 + digit;
	}
	if (value < (unsigned)min) {
		return -1;
	}
	*out = (int)value;
	return 0;
}

static inline int task_arg_bool(const char *s, int *out)
{
	if (task_arg_equal(s, "1") || task_arg_equal(s, "yes") || task_arg_equal(s, "true")) {
		*out = 1;
		return 0;
	}
	if (task_arg_equal(s, "0") || task_arg_equal(s, "no") || task_arg_equal(s, "false")) {
		*out = 0;
		return 0;
	}
	return -1;
}
#endif
