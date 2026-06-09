/*
 * debug.c - 调试辅助函数
 *
 * 提供 switch_to 路径的寄存器/栈诊断与 backtrace 追踪。
 * 不参与内核主线逻辑，主要防调试时翻车时用。
 */

#include <limine.h>

#include "ksym.h"
#include "debug.h"
#include "printf.h"

// 停机函数。
void hcf(void)
{
	for (;;) {
#if defined (__x86_64__)
		asm ("hlt");
#elif defined (__aarch64__) || defined (__riscv)
		asm ("wfi");
#elif defined (__loongarch64)
		asm ("idle 0");
#endif
	}
}
EXPORT_SYMBOL(hcf);

void debug_switch_to_rsp(uint64_t current_rsp, uint64_t top_value, uint64_t is_user)
{
	kprintf("[switch_to] new rsp = %p, top_value (即将ret) = %p, is_user=%ld\n",
			(void*)current_rsp, (void*)top_value, is_user);
}

void debug_before_ret(uint64_t ret_addr)
{
	kprintf("[switch_to] ABOUT TO RET to %p\n", (void*)ret_addr);
	// 如果更狠一点，可以在这里加条件断点风格的停顿
}

void debug_common_stub_entry(uint64_t entry_rip)
{
	kprintf("[common_stub ENTRY] RIP = %p\n", (void*)entry_rip);
}

/*
 * x86_64 调用约定：
 *   - 栈帧结构(开启 -fno-omit-frame-pointer 时):
 *
 *     高地址
 *     +-------------+
 *     |  返回地址    | <- (rbp+8)
 *     |  saved rbp  | <- rbp  <- 当前帧基址
 *     |  局部变量    |
 *     +-------------+
 *     低地址
 *
 * 注意：必须在编译时加 -fno-omit-frame-pointer,
 *       否则 RBP 不保证作为帧指针使用，回溯会出错。
 */

/* 符号表查找接口(需要内核实现，用于将地址转成函数名)*/
extern const char *kallsyms_lookup(unsigned long addr, unsigned long *offset);

#define STACK_TRACE_DEPTH_MAX  32   /* 最多回溯层数 */

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
		/*
		 * 合法性检查：
		 * 1. rbp 必须在内核栈范围内(需要提供 is_kernel_stack_addr())
		 * 2. rbp 必须8字节对齐
		 */
		if (!rbp || ((unsigned long)rbp & 0x7) != 0) {
			break;
		}
		if (!is_kernel_stack_addr((unsigned long)rbp)) {
			break;
		}

		unsigned long ret_addr = *(rbp + 1);   /* rbp+8 存放返回地址 */

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
