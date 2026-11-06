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

/* 初始线程上下文将入口函数地址保存在 R15，thread_entry_point 从此处读取。 */
static inline void *arch_get_entry_func(void)
{
	void *func;
	__asm__ volatile("mov %%r15, %0" : "=r"(func));
	return func;
}

/* GS 基址指向 cpu_context；offset 是待读取字段的字节偏移。 */
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

/* 返回初始上下文中保存的入口函数；此架构忽略通用接口传入的 entry_func。 */
static inline void *arch_thread_get_entry(void *entry_func)
{
	(void)entry_func;
	return arch_get_entry_func();
}

static inline uint64_t arch_thread_init_frame(uint64_t stack_top, void *entry, void *data)
{
	uint64_t *rsp = (uint64_t *)((uintptr_t)stack_top);
	if (((uint64_t)rsp & 0xF) != 0) {
		/* 初始内核栈顶必须按 16 字节对齐。 */
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

/* 构造用户线程首次切换所需的内核上下文帧。 */
struct thread;
struct thread;
void arch_user_thread_entry_stub(struct thread *t);

static inline uint64_t arch_thread_init_user_frame(struct thread *t, uint64_t stack_top)
{
	uint64_t *rsp = (uint64_t *)stack_top;
	rsp -= 15; /* 为入口存根构造 iretq 所需的局部栈帧预留空间。 */
	*(--rsp) = (uint64_t)arch_user_thread_entry_stub;
	*(--rsp) = 0x202;	/* rflags (IF=1) */
	*(--rsp) = 0;		/* rbp */
	*(--rsp) = 0;		/* rbx */
	*(--rsp) = 0;		/* r12 */
	*(--rsp) = 0;		/* r13 */
	*(--rsp) = 0;		/* r14 */
	*(--rsp) = 0;		/* r15 */
	*(--rsp) = (uint64_t)t; /* rdi */
	return (uint64_t)rsp;
}

#endif
