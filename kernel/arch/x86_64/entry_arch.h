/*
 * entry_arch.h — 用户态进入/退出 (x86_64)
 *
 * 提供：
 *   arch_enter_usermode(stack_ptr)
 *     — 从内核态进入 ELF 用户进程。stack_ptr 指向已布置好的
 *       [RIP][CS][RFLAGS][RSP][SS] iretq 帧。
 *       swapgs + iretq 切换到 Ring 3。
 */

#ifndef __ARCH_X86_64_ENTRY_ARCH_H__
#define __ARCH_X86_64_ENTRY_ARCH_H__

#include <stdint.h>

/*
 * x86_64 进入用户态：
 *   1. 在栈上布置 iretq 帧 (SS, RSP, RFLAGS, CS, RIP)
 *   2. 切换数据段选择子 (DS, ES)
 *   3. swapgs 切换 GS 基址
 *   4. iretq 恢复用户态指令指针、栈和标志寄存器
 *
 * rsp_ptr：指向已布置好 iretq 帧的内核栈指针。
 *   帧格式：[RIP][CS][RFLAGS][RSP][SS]
 *   调用方负责在 iretq 帧下方留出至少 64 字节余地。
 */
static inline __attribute__((noreturn)) void arch_enter_usermode(void *stack_ptr)
{
	/* 设置用户态数据段选择子，切换 GS 基址并从预制帧进入 Ring 3。
	 * 调用方须保证进入此序列前本地 IRQ 已关闭。 */
	__asm__ volatile("mov $0x23, %%ax\n\t"
			 "mov %%ax, %%ds\n\t"
			 "mov %%ax, %%es\n\t"
			 "swapgs\n\t"
			 "mov %0, %%rsp\n\t"
			 "iretq"
		:
		: "r"((uint64_t)stack_ptr)
		: "ax", "memory");

	__builtin_unreachable();
}

#endif
