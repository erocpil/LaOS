/* x86_64 用户态系统调用封装。系统调用号放入 RAX，参数依次使用
 * RDI、RSI、RDX、R10、R8、R9；SYSCALL 会覆盖 RCX 和 R11。 */
static inline long syscall6(
	long nr, long arg1, long arg2, long arg3, long arg4, long arg5, long arg6)
{
	long ret;
	asm volatile("movq %5, %%r10\n\t" // SYSCALL ABI 将第 4 个参数放在 R10，而非 RCX。
		     "movq %6, %%r8\n\t"
		     "movq %7, %%r9\n\t"
		     "syscall"
		: "=a"(ret)
		: "a"(nr), "D"(arg1), "S"(arg2), "d"(arg3), "g"(arg4), "g"(arg5), "g"(arg6)
		: "rcx", "r11", "memory" // 声明 SYSCALL 覆盖的寄存器及内存副作用。
	);
	return ret;
}

__attribute__((noreturn)) void exit(int status)
{
	/* SYS_EXIT = 60。 */
	syscall6(60, (long)status, 0, 0, 0, 0, 0);

	// 若内核意外返回，避免继续执行调用方代码。
	while (1)
		;
	__builtin_unreachable();
}

int write(int fd, const void *buf, unsigned long count)
{
	/* SYS_WRITE = 1。 */
	int ret = (int)syscall6(1, (long)fd, (long)buf, (long)count, 0, 0, 0);
	return ret;
}

int msleep(unsigned int msec)
{
	/* SYS_SLEEP = 35。 */
	return (int)syscall6(35, (long)msec, 0, 0, 0, 0, 0);
}

void yield(void)
{
	syscall6(3, 0, 0, 0, 0, 0, 0);
}

void *mmap(unsigned long length, unsigned long prot, unsigned long flags)
{
	/* SYS_MMAP 的参数依次为 length、prot、flags。 */
	return (void *)syscall6(5, (long)length, (long)prot, (long)flags, 0, 0, 0);
}

int munmap(unsigned long addr, unsigned long length)
{
	/* SYS_MUNMAP 的参数依次为 addr、length。 */
	return (int)syscall6(6, (long)addr, (long)length, 0, 0, 0, 0);
}
