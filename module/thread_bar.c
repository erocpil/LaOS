/* thread_bar.c - bar 测试线程：打印 10 轮消息后等待 10 秒自动退出 */

static void outb(unsigned short port, unsigned char val) {
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void _start()
{
	char *msg = "Hello from thread bar!\n";
	for (int n = 0; n < 10; n++) {
		outb(0x3f8, '0' + n);
		outb(0x3f8, ':');
		outb(0x3f8, ' ');
		for (int i = 0; msg[i] != '\0'; i++) {
			outb(0x3f8, msg[i]);
		}
	}

	/* 10 秒后自动退出：LaOS 定时器 ~100Hz，1000 次 hlt ≈ 10s。
	 * 从 _start 返回后 thread_entry_point 会调用 thread_exit(code) 正确终止线程。 */
	for (int i = 0; i < 1000; i++)
		__asm__ volatile("hlt");
}
