/* ARM64 用户态系统调用封装。
 * SVC 立即数用于选择系统调用；参数和返回值通过 x0–x5 传递。具体编号与
 * 参数约定应与 kernel/arch/aarch64/syscall.h 保持一致。
 */

#include <stdint.h>

static inline long svc0(long a0, long a1, long a2, long a3, long a4, long a5)
{
	register long x0_ asm("x0") = a0;
	register long x1 asm("x1") = a1;
	register long x2 asm("x2") = a2;
	register long x3 asm("x3") = a3;
	register long x4 asm("x4") = a4;
	register long x5 asm("x5") = a5;
	register long ret asm("x0");

	asm volatile("svc #0\n"
		: "=r"(ret)
		: "r"(x0_), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
		: "memory");
	return ret;
}

static inline long svc1(long a0)
{
	register long x0_ asm("x0") = a0;
	register long ret asm("x0");
	asm volatile("svc #1\n" : "=r"(ret) : "r"(x0_) : "memory");
	return ret;
}

/* 用户程序使用的系统调用接口。 */

__attribute__((noreturn)) void exit(int status)
{
	svc1((long)status);
	while (1)
		;
	__builtin_unreachable();
}

int write(int fd, const void *buf, unsigned long count)
{
	(void)fd;
	return (int)svc0((long)fd, (long)buf, (long)count, 0, 0, 0);
}

int msleep(unsigned int msec)
{
	// 当前实现仍调用内核占位系统调用，休眠功能尚未接入。
	return (int)svc0((long)msec, 0, 0, 0, 0, 0);
}
