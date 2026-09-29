/*
 * tty_test.c - TTY 多路复用自检测试
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

#include "tty_test.h"

#if CONFIG_TTY_TEST

static void tty_test_thread(void *data)
{
	(void)data;

	kprintf("TTY-TEST: starting 6 scenarios...\n");

	int fails = 0;
	struct thread *t = get_current();

	// 1. TTY 0 is default: any thread may write
	t->tty_id = 0;
	atomic_set(&current_tty_id, 0);
	if (!tty_ready()) {
		kprintf("TTY-TEST: FAIL 1 (TTY 0, tty_id=0, expected ready)\n");
		fails++;
	}

	// 2. TTY 0 + thread with bit 6: still ready
	t->tty_id = (1 << 6);
	atomic_set(&current_tty_id, 0);
	if (!tty_ready()) {
		kprintf("TTY-TEST: FAIL 2 (TTY 0, tty_id=bit6, expected ready)\n");
		fails++;
	}

	// 3. TTY 6 + thread with bit 6: allowed
	t->tty_id = (1 << 6);
	atomic_set(&current_tty_id, 6);
	if (!tty_ready()) {
		kprintf("TTY-TEST: FAIL 3 (TTY 6, tty_id=bit6, expected ready)\n");
		fails++;
	}

	// 4. TTY 6 + thread with only bit 7: blocked
	t->tty_id = (1 << 7);
	atomic_set(&current_tty_id, 6);
	if (tty_ready()) {
		kprintf("TTY-TEST: FAIL 4 (TTY 6, tty_id=bit7, expected blocked)\n");
		fails++;
	}

	// 5. TTY 6 + thread with tty_id=0: blocked
	t->tty_id = 0;
	atomic_set(&current_tty_id, 6);
	if (tty_ready()) {
		kprintf("TTY-TEST: FAIL 5 (TTY 6, tty_id=0, expected blocked)\n");
		fails++;
	}

	// 6. Uninitialized: blocked
	atomic_set(&current_tty_id, -1);
	if (tty_ready()) {
		kprintf("TTY-TEST: FAIL 6 (uninit, expected blocked)\n");
		fails++;
	}

	// Restore
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

void tty_test_run(void) { /* no-op */ }

#endif
