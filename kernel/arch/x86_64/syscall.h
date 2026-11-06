#ifndef __SYSCALL_H__
#define __SYSCALL_H__

/*
 * x86_64 系统调用编号、MSR 地址与保存帧定义。
 */

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

#define SYS_WRITE  1  /* write(fd, buf, count) */
#define SYS_YIELD  3  /* 主动让出 CPU */
#define SYS_MMAP   5  /* mmap(length, prot, flags) */
#define SYS_MUNMAP 6  /* munmap(addr, length) */
#define SYS_SLEEP  35 /* msleep(msec) — 休眠指定毫秒数 */
#define SYS_EXIT   60 /* exit(status) — 终止当前线程 */

#include <stdint.h>

#include "debug.h"

struct trap_frame {
	/* syscall_entry.asm 按此布局保存的通用寄存器。 */
	uint64_t rax; /* 系统调用号。 */
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t r9;
	uint64_t r8;
	uint64_t r10; /* x86_64 系统调用 ABI 的第四个参数。 */
	uint64_t rdx;
	uint64_t rsi;
	uint64_t rdi; /* 第一个参数。 */
	uint64_t rbx;
	uint64_t rbp;

	/* syscall_entry.asm 根据 SYSCALL 的寄存器约定构造的返回状态。 */
	uint64_t rip; /* 用户态返回地址，由 RCX 提供。 */
	uint64_t cs;
	uint64_t rflags; /* 用户态 RFLAGS，由 R11 提供。 */
	uint64_t rsp;	 /* 用户态栈指针。 */
	uint64_t ss;
};

#define trap_frame_dump(t)                                                                         \
	do {                                                                                       \
		__typeof__(t) _t = (t);                                                            \
		L("CPU %d %s %ld trap frame %p\n"                                                  \
		  "  rax      %p %lu\n"                                                            \
		  "  r12      %p %lu\n"                                                            \
		  "  r13      %p %lu\n"                                                            \
		  "  r14      %p %lu\n"                                                            \
		  "  r15      %p %lu\n"                                                            \
		  "  r9       %p %lu\n"                                                            \
		  "  r8       %p %lu\n"                                                            \
		  "  r10      %p %lu\n"                                                            \
		  "  rdx      %p %lu\n"                                                            \
		  "  rsi      %p %lu\n"                                                            \
		  "  rdi      %p %lu\n"                                                            \
		  "  rbx      %p %lu\n"                                                            \
		  "  rbp      %p %lu\n"                                                            \
		  "  rip      %p %lu\n"                                                            \
		  "  cs       %p %lu\n"                                                            \
		  "  rflags   %p %lu\n"                                                            \
		  "  rsp      %p %lu\n"                                                            \
		  "  ss       %p %lu\n",                                                           \
			cpu_get_ctx()->id, cpu_get_ctx()->current->name,                           \
			cpu_get_ctx()->current->id, _t, (void *)_t->rax, _t->rax, (void *)_t->r12, \
			_t->r12, (void *)_t->r13, _t->r13, (void *)_t->r14, _t->r14,               \
			(void *)_t->r15, _t->r15, (void *)_t->r9, _t->r9, (void *)_t->r8, _t->r8,  \
			(void *)_t->r10, _t->r10, (void *)_t->rdx, _t->rdx, (void *)_t->rsi,       \
			_t->rsi, (void *)_t->rdi, _t->rdi, (void *)_t->rbx, _t->rbx,               \
			(void *)_t->rbp, _t->rbp, (void *)_t->rip, _t->rip, (void *)_t->cs,        \
			_t->cs, (void *)_t->rflags, _t->rflags, (void *)_t->rsp, _t->rsp,          \
			(void *)_t->ss, _t->ss);                                                   \
	} while (0)

void syscall_init(void);

#endif
