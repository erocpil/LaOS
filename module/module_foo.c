#include "../kernel/sched.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/printf.h"

#include "task_args.h"

struct foo_config {
	int count;
	int tick;
};

static int foo_parse_config(int argc, char *argv[], struct foo_config *config)
{
	*config = (struct foo_config){.count = 1, .tick = 1};
	for (int i = 1; i < argc; i++) {
		const char *value;
		if ((value = task_arg_value(argv[i], "count"))) {
			if (task_arg_uint(value, 1, 1000, &config->count)) {
				return -1;
			}
		} else if ((value = task_arg_value(argv[i], "tick"))) {
			if (task_arg_uint(value, 1, 1000000, &config->tick)) {
				return -1;
			}
		} else {
			return -1;
		}
	}
	return 0;
}

int laos_module_entry(int argc, char *argv[])
{
	struct foo_config config;
	if (foo_parse_config(argc, argv, &config)) {
		kprintf("[module-foo] invalid arguments\n");
		return -1;
	}
	const int count = config.count;
	const int tick = config.tick;

	char *msg = "message from module foo!";

	if (count == 2 && tick == 10) {
		kprintf("[module-foo] started: count=2 tick=10\n");
	}

	L("[%s %s %d] %s", __FILE__, __func__, __LINE__, msg);

	kprintf("%s\n", msg);

	if (interrupts_enabled()) {
		L("[%s] IF=1, interrupt on", __FILE__);
	} else {
		L("[%s] IF=0, interrupt off", __FILE__);
	}

	int n = 0;
	int d = 0;
	int resumed = 0;
	while (1) {
		if (!(n++ % (1000 / count))) {
			d++;
			L("[%s %s %d] %s cpu=%d d=%d", __FILE__, __func__, __LINE__, msg,
				cpu_get_ctx()->id, d);
		}
		schedule_timeout(tick);
		if (!resumed) {
			kprintf("[module-foo] resumed after timeout\n");
			resumed = 1;
		}
	}

	thread_exit(0);
	return 0;
}
