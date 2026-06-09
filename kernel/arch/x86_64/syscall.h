#ifndef __SYSCALL_H__
#define __SYSCALL_H__

/*
 * syscall.h - syscall 编号，MSR 地址，入口声明
 */

#define MSR_EFER       0xC0000080
#define MSR_STAR       0xC0000081
#define MSR_LSTAR      0xC0000082
#define MSR_SFMASK     0xC0000084

#define SYS_WRITE      1        /* write(fd, buf, count) */
#define SYS_YIELD      3        /* 主动让出 CPU */
#define SYS_MMAP       5        /* mmap(length, prot, flags) */
#define SYS_MUNMAP     6        /* munmap(addr, length) */
#define SYS_SLEEP       35      /* msleep(msec) — 休眠指定毫秒数 */
#define SYS_EXIT        60      /* exit(status) — 终止当前线程 */

#include <stdint.h>

#include "debug.h"

struct trap_frame {
	// 由 syscall_entry_asm 手动压入的通用寄存器 (顺序对应汇编 push)
	uint64_t rax;    // 系统调用号
	uint64_t r9;
	uint64_t r8;
	uint64_t r10; // 用户态的 rcx 传入这里
	uint64_t rdx;
	uint64_t rsi;
	uint64_t rdi;    // 第一个参数
	uint64_t rbx;
	uint64_t rbp;

	// 由 syscall 指令或手动构造的"伪中断栈帧"
	uint64_t rip;    // 用户态返回地址 (原本在 rcx)
	uint64_t cs;
	uint64_t rflags; // 用户态状态 (原本在 r11)
	uint64_t rsp;    // 用户态栈指针
	uint64_t ss;
};

#define trap_frame_dump(t) \
	do { \
		__typeof__(t) _t = (t); \
		L("CPU %d %s %ld trap frame %p\n" \
				"  rax      %p %lu\n" \
				"  r9       %p %lu\n" \
				"  r8       %p %lu\n" \
				"  r10      %p %lu\n" \
				"  rdx      %p %lu\n" \
				"  rsi      %p %lu\n" \
				"  rdi      %p %lu\n" \
				"  rbx      %p %lu\n" \
				"  rbp      %p %lu\n" \
				"  rip      %p %lu\n" \
				"  cs       %p %lu\n" \
				"  rflags   %p %lu\n" \
				"  rsp      %p %lu\n" \
				"  ss       %p %lu\n", \
				cpu_get_ctx()->id, cpu_get_ctx()->current->name, cpu_get_ctx()->current->id, _t, \
				(void*)_t->rax, _t->rax, (void*)_t->r9, _t->r9, (void*)_t->r8, _t->r8, \
				(void*)_t->r10, _t->r10, (void*)_t->rdx, _t->rdx, (void*)_t->rsi, _t->rsi, \
				(void*)_t->rdi, _t->rdi, (void*)_t->rbx, _t->rbx, (void*)_t->rbp, _t->rbp, \
				(void*)_t->rip, _t->rip, (void*)_t->cs, _t->cs, (void*)_t->rflags, _t->rflags, \
				(void*)_t->rsp, _t->rsp, (void*)_t->ss, _t->ss); \
	} while (0)

void syscall_init(void);

#endif
