// user_lib.c 或直接写在用户 ELF 源码中

// 使用 GCC/Clang 内建属性
// __attribute__((noreturn)) void exit(int status)
// {
// 	asm volatile(
// 			"mov $60, %%rax\n"    // SYS_EXIT 的调用号是 60
// 			"mov %0, %%rdi\n"     // 第一个参数：退出状态码码
// 			"syscall"             // 触发跳转到内核 LSTAR
// 			:
// 			: "r"((unsigned long)status)
// 			: "rax", "rdi"
// 			);
// 	// while (1);
// 	// 告诉编译器：程序永远不会运行到这里
// 	__builtin_unreachable();
// }

// 通用系统调用宏 (最多支持6个参数)
// 根据 x86_64 ABI:
// syscall No. -> rax
// 参数 -> rdi， rsi, rdx, r10, r8, r9
// 被破坏 -> rcx， r11(硬件强制)
static inline long syscall6(long nr, long arg1, long arg2, long arg3,
		long arg4, long arg5, long arg6)
{
	long ret;
	asm volatile (
			"movq %5, %%r10\n\t"  // ABI 规定第4个参数使用 r10 而非 rcx
			"movq %6, %%r8\n\t"
			"movq %7, %%r9\n\t"
			"syscall"
			: "=a" (ret)
			: "a" (nr), "D" (arg1), "S" (arg2), "d" (arg3),
			"g" (arg4), "g" (arg5), "g" (arg6)
			: "rcx", "r11", "memory"  // 关键修复：加入 rcx， r11 和内存屏障
			);
	return ret;
}

// 修复后的 exit 函数
__attribute__((noreturn)) void exit(int status)
{
	// SYS_EXIT = 60
	syscall6(60, (long)status, 0, 0, 0, 0, 0);

	// 理论上不会运行到这里
	while (1);
	__builtin_unreachable();
}

// 新增：修复后的 write 函数示例
// 展示了为什么 rcx/r11 保护对可返回的调用至关重要
int write(int fd, const void *buf, unsigned long count)
{
	// SYS_WRITE = 1
	int ret =  (int)syscall6(1, (long)fd, (long)buf, (long)count, 0, 0, 0);
	return ret;
}

int msleep(unsigned int msec)
{
	// SYS_SLEEP = 35
	return (int)syscall6(35, (long)msec, 0, 0, 0, 0, 0);
}

void yield(void)
{
	syscall6(3, 0, 0, 0, 0, 0, 0);
}

void *mmap(unsigned long length, unsigned long prot, unsigned long flags)
{
	/* SYS_MMAP = 5: length=rdi, prot=rsi, flags=rdx */
	return (void *)syscall6(5, (long)length, (long)prot, (long)flags, 0, 0, 0);
}

int munmap(unsigned long addr, unsigned long length)
{
	/* SYS_MUNMAP = 6: addr=rdi, length=rsi */
	return (int)syscall6(6, (long)addr, (long)length, 0, 0, 0, 0);
}
