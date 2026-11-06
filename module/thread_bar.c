/* 测试线程：输出 10 轮消息后休眠约 10 秒并退出。 */

static void outb(unsigned short port, unsigned char val)
{
	__asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
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

	/* 计时器频率约为 100 Hz，休眠 1000 个 tick 后结束线程。入口返回后，
	 * thread_entry_point() 会调用 thread_exit() 完成线程退出。 */
	for (int i = 0; i < 1000; i++) {
		__asm__ volatile("hlt");
	}
}
