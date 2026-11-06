/*
 * x86_64 调试辅助函数与栈回溯。
 *
 * 提供上下文切换诊断输出、停机辅助函数和基于帧指针的调用栈回溯。
 */

#include "ksym.h"
#include "debug.h"
#include "printf.h"

// 停机函数。
void hcf(void)
{
	for (;;) {
#if defined(__x86_64__)
		asm("hlt");
#elif defined(__aarch64__) || defined(__riscv)
		asm("wfi");
#elif defined(__loongarch64)
		asm("idle 0");
#endif
	}
}
EXPORT_SYMBOL(hcf);

void debug_switch_to_rsp(uint64_t current_rsp, uint64_t top_value, uint64_t is_user)
{
	kprintf("[switch_to] new rsp = %p, top_value (即将ret) = %p, is_user=%ld\n",
		(void *)current_rsp, (void *)top_value, is_user);
}

void debug_before_ret(uint64_t ret_addr)
{
	kprintf("[switch_to] ABOUT TO RET to %p\n", (void *)ret_addr);
}

void debug_common_stub_entry(uint64_t entry_rip)
{
	kprintf("[common_stub ENTRY] RIP = %p\n", (void *)entry_rip);
}

/*
 * x86_64 调用约定：
 *   - 栈帧结构(开启 -fno-omit-frame-pointer 时):
 *
 *     高地址
 *     +-------------+
 *     |  返回地址   | <- (rbp+8)
 *     |  saved rbp  | <- rbp  <- 当前帧基址
 *     |  局部变量   |
 *     +-------------+
 *     低地址
 *
 * 注意：必须在编译时加 -fno-omit-frame-pointer,
 *       否则 RBP 不保证作为帧指针使用，回溯会出错。
 */

/* 符号表查找接口(需要内核实现，用于将地址转成函数名)*/
extern const char *kallsyms_lookup(unsigned long addr, unsigned long *offset);

#define STACK_TRACE_DEPTH_MAX 32 /* 最多回溯层数 */

static inline int is_kernel_stack_addr(uint64_t rbp)
{
	if (rbp >= 0xffffffff80000000) {
		return 1;
	}

	return 0;
}

void dump_stack(void)
{
	unsigned long *rbp;

	/* 读取当前 RBP */
	asm volatile("mov %%rbp, %0" : "=r"(rbp));

	kprintf("Call Trace:\n");

	for (int depth = 0; depth < STACK_TRACE_DEPTH_MAX; depth++) {
		/* 仅检查地址下界和对齐；这不足以证明栈帧映射有效或属于当前栈。 */
		if (!rbp || ((unsigned long)rbp & 0x7) != 0) {
			break;
		}
		if (!is_kernel_stack_addr((unsigned long)rbp)) {
			break;
		}

		unsigned long ret_addr = *(rbp + 1); /* rbp+8 存放返回地址 */

		if (!ret_addr) {
			break;
		}

		/* 尝试符号解析 */
		unsigned long offset = 0;
		const char *sym = kallsyms_lookup(ret_addr, &offset);

		if (sym) {
			kprintf("  [<%016lx>] %s+0x%lx\n", ret_addr, sym, offset);
		} else {
			kprintf("  [<%016lx>] (unknown)\n", ret_addr);
		}

		/* 跳到上一帧 */
		rbp = (unsigned long *)*rbp; /* *rbp = saved rbp of caller */
	}
}
