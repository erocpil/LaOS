#include <stddef.h>

#include "cpu.h"
#include "idt.h"
#include "gdt.h"
/*
 * 核心宏：利用 asm 打印特殊的标记模式 "-> @@@"
 * 方便后续用 sed/awk 抓取
 */
// 使用汇编注释符号 # 替代 ->
// 这样即使被汇编器扫描，它也会认为这是一行注释而忽略
#define DEFINE(sym, val) \
	asm volatile("\n#ASM_OFFSET# " #sym " %0 " #val : : "i" (val))

#define OFFSET(sym, str, mem) \
	DEFINE(sym, offsetof(struct str, mem))

/*
 * 计算成员到结构体尾部的偏移：结构体总大小 - 成员起始偏移 - 成员自身大小
 */
#define OFFSET_FROM_END(sym, str, mem) \
	DEFINE(sym, (sizeof(struct str) - offsetof(struct str, mem) - sizeof(((struct str *)0)->mem)))

void generate_offsets(void)
{
	OFFSET(CPU_CTX_CURRENT, cpu_context, current);
	OFFSET(CPU_CTX_USER_RSP, cpu_context, user_rsp);
	OFFSET(INTERRUPT_FRAME_CS, interrupt_frame, cs);
	OFFSET_FROM_END(INTERRUPT_FRAME_CS_END, interrupt_frame, cs);
	OFFSET(THREAD_KSTACK, thread, kernel_stack);
	OFFSET(THREAD_FPU_STATE, thread, fpu_state);
	DEFINE(USER_CS_SEL, USER_CS);
	DEFINE(USER_SS_SEL, USER_SS);
}
