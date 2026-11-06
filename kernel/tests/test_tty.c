/*
 * test_tty.c - TTY 多路复用自检测试
 *
 * 验证 tty_ready() 的 6 种场景：
 *   1. TTY 0 (default)     -- 任意线程可输出
 *   2. TTY 0 + 线程有 bit 6 -- 仍可输出
 *   3. TTY 6 + 线程有 bit 6 -- 放行
 *   4. TTY 6 + 线程只有 bit 7 -- 拒绝
 *   5. TTY 6 + 线程 tty_id=0  -- 拒绝
 *   6. current_tty_id == -1  -- 拒绝
 *
 * 测试线程创建后把结果打印到串口，不受 TTY 切换影响。
 */

#include "test_tty.h"

#if CONFIG_TTY_TEST

static void tty_test_thread(void *data)
{
	(void)data;

	kprintf("TTY-TEST: starting 6 scenarios...\n");

	int fails = 0;
	struct thread *t = get_current();

	// 默认终端 0 允许任意线程输出。
	t->tty_id = 0;
	atomic_set(&current_tty_id, 0);
	if (!tty_ready()) {
		kprintf("TTY-TEST: FAIL 1 (TTY 0, tty_id=0, expected ready)\n");
		fails++;
	}

	// 当前终端为 0 时，线程的终端权限位不影响输出。
	t->tty_id = (1 << 6);
	atomic_set(&current_tty_id, 0);
	if (!tty_ready()) {
		kprintf("TTY-TEST: FAIL 2 (TTY 0, tty_id=bit6, expected ready)\n");
		fails++;
	}

	// 当前终端为 6，且线程具有对应权限时，允许输出。
	t->tty_id = (1 << 6);
	atomic_set(&current_tty_id, 6);
	if (!tty_ready()) {
		kprintf("TTY-TEST: FAIL 3 (TTY 6, tty_id=bit6, expected ready)\n");
		fails++;
	}

	// 当前终端为 6，而线程只有终端 7 的权限时，拒绝输出。
	t->tty_id = (1 << 7);
	atomic_set(&current_tty_id, 6);
	if (tty_ready()) {
		kprintf("TTY-TEST: FAIL 4 (TTY 6, tty_id=bit7, expected blocked)\n");
		fails++;
	}

	// 当前终端为 6，而线程没有终端权限时，拒绝输出。
	t->tty_id = 0;
	atomic_set(&current_tty_id, 6);
	if (tty_ready()) {
		kprintf("TTY-TEST: FAIL 5 (TTY 6, tty_id=0, expected blocked)\n");
		fails++;
	}

	// 当前终端尚未初始化时，拒绝输出。
	atomic_set(&current_tty_id, -1);
	if (tty_ready()) {
		kprintf("TTY-TEST: FAIL 6 (uninit, expected blocked)\n");
		fails++;
	}

	// 恢复测试前的默认终端状态。
	atomic_set(&current_tty_id, 0);

	if (fails == 0) {
		kprintf("TTY-TEST: all 6 passed\n");
	} else {
		kprintf("TTY-TEST: %d FAILURE(S)\n", fails);
	}
}

void tty_test_run(void)
{
	kthread_create_on("tty_test", tty_test_thread, NULL, 0);
}

#else /* !CONFIG_TTY_TEST */

void tty_test_run(void)
{ /* no-op */
}

#endif
