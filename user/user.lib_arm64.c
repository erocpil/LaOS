// user_lib_arm64.c — ARM64 用户态系统调用包装
//
// SVC 约定（匹配 kernel/arch/aarch64/syscall.h）：
//   SVC immediate 即为 syscall 号。参数 x0-x5，返回值 x0。
//   svc #0  = SYS_WRITE (x0=fd, x1=buf, x2=len)
//   svc #1  = SYS_EXIT  (x0=code)
//   svc #2  = SYS_TEST  (无参数)

#include <stdint.h>

static inline long svc0(long a0, long a1, long a2,
                         long a3, long a4, long a5)
{
    register long x0_ asm("x0") = a0;
    register long x1  asm("x1") = a1;
    register long x2  asm("x2") = a2;
    register long x3  asm("x3") = a3;
    register long x4  asm("x4") = a4;
    register long x5  asm("x5") = a5;
    register long ret asm("x0");

    asm volatile (
        "svc #0\n"
        : "=r"(ret)
        : "r"(x0_), "r"(x1), "r"(x2), "r"(x3),
          "r"(x4), "r"(x5)
        : "memory"
    );
    return ret;
}

static inline long svc1(long a0) {
    register long x0_ asm("x0") = a0;
    register long ret asm("x0");
    asm volatile ("svc #1\n" : "=r"(ret) : "r"(x0_) : "memory");
    return ret;
}

// ---- 公共接口 -------------------------------------------------------

__attribute__((noreturn)) void exit(int status)
{
    svc1((long)status);
    while (1);
    __builtin_unreachable();
}

int write(int fd, const void *buf, unsigned long count)
{
    (void)fd;
    return (int)svc0((long)fd, (long)buf, (long)count, 0, 0, 0);
}

int msleep(unsigned int msec)
{
    // 通过 svc #2 占位，内核侧为 stub
    return (int)svc0((long)msec, 0, 0, 0, 0, 0);
}
