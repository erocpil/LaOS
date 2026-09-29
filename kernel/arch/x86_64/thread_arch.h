/*
 * thread_arch.h — 线程相关的架构抽象 (x86_64)
 *
 * 提供：
 *   arch_get_entry_func()  — 从架构约定的位置取出线程入口函数
 *   arch_get_percpu_ptr()  — 读取 per-CPU 指针 (x86: GS 段)
 */

#ifndef __ARCH_X86_64_THREAD_ARCH_H__
#define __ARCH_X86_64_THREAD_ARCH_H__

#include <stdint.h>

extern uint8_t fpu_initial_state[576];
extern void ret_from_fork(void);

/* 旧路径：R15 寄存在 switch.asm 中保存的入口函数。
 * switch_to 在跳转到新线程前将 func 写入 R15；
 * thread_entry_point 通过此函数读出。 */
static inline void *arch_get_entry_func(void)
{
	void *func;
	__asm__ volatile("mov %%r15, %0" : "=r"(func));
	return func;
}

/* x86_64 per-CPU：GS 段基址指向 cpu_context。
 * offsetof(cpu_context, current) == 16。 */
static inline uint64_t arch_get_percpu_offset(uint16_t offset)
{
	uint64_t val;
	__asm__ volatile("movq %%gs:(%1), %0" : "=r"(val) : "r"((uint64_t)offset));
	return val;
}

static inline void arch_fpu_reset_state(void *fpu_state)
{
	__builtin_memcpy(fpu_state, fpu_initial_state, 576);
}

/** x86_64: switch.S 通过 R15 走私入口函数，忽略参数直接从 R15 读 */
static inline void *arch_thread_get_entry(void *entry_func)
{
	(void)entry_func;
	return arch_get_entry_func();
}

static inline uint64_t arch_thread_init_frame(uint64_t stack_top, void *entry, void *data)
{
	uint64_t *rsp = (uint64_t *)((uintptr_t)stack_top);
	if (((uint64_t)rsp & 0xF) != 0) {
		/* alignment violated — caller must ensure 16-byte stack alignment */
		__builtin_trap();
	}
	*(--rsp) = 0;
	*(--rsp) = (uint64_t)ret_from_fork;
	*(--rsp) = 0x202;
	*(--rsp) = 0;
	*(--rsp) = 0;
	*(--rsp) = 0;
	*(--rsp) = 0;
	*(--rsp) = 0;
	*(--rsp) = (uint64_t)entry;
	*(--rsp) = (uint64_t)data;
	return (uint64_t)rsp;
}


struct thread;
struct thread;
void arch_user_thread_entry_stub(struct thread *t);

/** arch_thread_init_user_frame -- x86_64 switch_to context frame for user threads */
struct thread;
struct thread;
void arch_user_thread_entry_stub(struct thread *t);

static inline uint64_t arch_thread_init_user_frame(struct thread *t, uint64_t stack_top)
{
	uint64_t *rsp = (uint64_t*)stack_top;
	rsp -= 15;  /* reserve space for stub's iretq locals */
	*(--rsp) = (uint64_t)arch_user_thread_entry_stub;
	*(--rsp) = 0x202;       /* rflags (IF=1) */
	*(--rsp) = 0;           /* rbp */
	*(--rsp) = 0;           /* rbx */
	*(--rsp) = 0;           /* r12 */
	*(--rsp) = 0;           /* r13 */
	*(--rsp) = 0;           /* r14 */
	*(--rsp) = 0;           /* r15 */
	*(--rsp) = (uint64_t)t; /* rdi */
	return (uint64_t)rsp;
}

#endif
