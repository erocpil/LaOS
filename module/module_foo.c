#include "../kernel/sched.h"
#include "../kernel/debug.h"
#include "../kernel/string.h"
#include "../kernel/module_param.h"
#include "../kernel/printf.h"

/* 模块参数 —— 由内核根据 task.conf 的 key=value 对预先写入 */
static int count = 1;
static int tick = 1;
MODULE_PARAM(count, INT, "print iterations per loop");
MODULE_PARAM(tick,  INT, "schedule_timeout tick interval");

static void serial_puts_unlocked(const char *s)
{
	while (*s)
		serial_putchar(*s++);
}

#if defined(__x86_64__)
static void _outb(unsigned short port, unsigned char val)
{
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
#endif

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	/* P1-1: count=0 from task.conf would cause divide-by-zero in 1000/count */
	if (count <= 0)
		count = 1;

	char *msg = "message from module foo!";

	if (count == 2 && tick == 10)
		serial_puts_unlocked("[module-foo] started: count=2 tick=10\n");
	else
		serial_puts_unlocked("[module-foo] parameter verification failed\n");
	L("[%s %s %d] %s", __FILE__, __func__, __LINE__, msg);

#if defined(__x86_64__)
	for (size_t i = 0; i < strlen(msg); i++)
		_outb(0x3f8, msg[i]);
#endif

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
			L("[%s %s %d] %s cpu=%d d=%d", __FILE__, __func__,
				__LINE__, msg, cpu_get_ctx()->id, d);
		}
		schedule_timeout(tick);
		if (!resumed) {
			serial_puts_unlocked("[module-foo] resumed after timeout\n");
			resumed = 1;
		}
	}

	thread_exit(0);
	return 0;
}
