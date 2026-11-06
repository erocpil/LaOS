/* 两个线程共用一个 ET_EXEC 映像，各自持有独立的 argv 和栈上配置。 */
#include "../task_args.h"

struct worker_config {
	int number;
	int enabled;
	char text[16];
};

static void report(const char *name, const char *result)
{
	unsigned long flags;
	__asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
	const char *parts[] = {"[task-args] ", name, result, "\n"};
	for (unsigned i = 0; i < 4; i++) {
		for (const char *s = parts[i]; *s; s++) {
			__asm__ volatile("outb %0, %1" : : "a"(*s), "Nd"((unsigned short)0x3f8));
		}
	}
	__asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

static int parse(int argc, char *argv[], struct worker_config *config)
{
	*config = (struct worker_config){.number = -1, .enabled = -1};
	if (argc != 5 || !task_arg_equal(argv[1], "positional") || argv[argc]) {
		return -1;
	}
	for (int i = 2; i < argc; i++) {
		const char *v;
		if ((v = task_arg_value(argv[i], "param_int"))) {
			if (task_arg_uint(v, 0, 1000, &config->number)) {
				return -1;
			}
		} else if ((v = task_arg_value(argv[i], "param_bool"))) {
			if (task_arg_bool(v, &config->enabled)) {
				return -1;
			}
		} else if ((v = task_arg_value(argv[i], "param_text"))) {
			unsigned n = 0;
			while (v[n]) {
				if (n == sizeof(config->text) - 1) {
					return -1;
				}
				config->text[n] = v[n];
				n++;
			}
			config->text[n] = 0;
		} else {
			return -1;
		}
	}
	return config->number < 0 || config->enabled < 0 || !config->text[0] ? -1 : 0;
}

int _start(int argc, char *argv[])
{
	struct worker_config config;
	if (parse(argc, argv, &config)) {
		int expected = task_arg_equal(argv[0], "params_bad");
		report(argv[0], expected ? " rejected" : " FAILED");
		return -1;
	}
	/* 通过定时让出使另一个实例运行，并检查本实例的配置仍保持独立。 */
	for (int i = 0; i < 20; i++) {
		__asm__ volatile("hlt" : : : "memory");
	}
	int first = task_arg_equal(argv[0], "params_a");
	int ok = (first || task_arg_equal(argv[0], "params_b")) &&
		 config.number == (first ? 77 : 88) && config.enabled == (first ? 1 : 0) &&
		 task_arg_equal(config.text, first ? "alpha" : "beta");
	report(argv[0], ok ? " PASSED" : " FAILED");
	return ok ? 0 : -1;
}
