#include "test_exception.h"

/*
 * test_exception.c -- 触发异常以验证异常处理器输出完整性
 *
 * 三层 noinline 调用链保证栈回溯至少显示 3 帧，每帧都能被 kallsyms_lookup 解析。
 * 由 CONFIG_EXCEPTION_TEST 开关控制，默认关闭。
 * 用 CONFIG_PF_TEST 选择异常类型： 0=#DE(除零)， 1=#PF(空指针)
 *
 * 验证项(#DE): GPR 全量 dump， CR0/CR4/RFLAGS 位解码， RIP 指令字节， 符号解析， 栈回溯
 * 验证项(#PF): CR0/CR4 位解码， #PF error code 解码， 四级页表遍历， 符号解析， 栈回溯
 */
#if CONFIG_EXCEPTION_TEST

#include "debug.h"
#include "export.h"
#include "printf.h"

/* 最内层：根据 CONFIG_PF_TEST 触发 #DE 或 #PF */
__attribute__((noinline)) void exception_test_trigger(void)
{
#if CONFIG_PF_TEST
	kprintf("  Triggering: page fault at 0x1000\n");
	volatile int *p = (volatile int *)0x1000;
	*p = 42; /* #PF: Page Fault */
#else
	volatile int a = 1;
	volatile int b = 0;

	kprintf("  Triggering: %d / %d = ...\n", a, b);
	volatile int c = a / b; /* #DE: Divide Error */
	(void)c;
#endif
}
EXPORT_SYMBOL(exception_test_trigger);

/* 中间层 */
__attribute__((noinline)) void exception_test_lv2(void)
{
	exception_test_trigger();
}
EXPORT_SYMBOL(exception_test_lv2);

/* 最外层：由 test_exception_run() 调用。 */
__attribute__((noinline)) void exception_test_lv1(void)
{
	exception_test_lv2();
}
EXPORT_SYMBOL(exception_test_lv1);

void test_exception_run(void)
{
	kprintf("\n");
	kprintf("========================================\n");
#if CONFIG_PF_TEST
	kprintf("  EXCEPTION HANDLER TEST: #PF (NULL deref)\n");
	kprintf("========================================\n");
	kprintf("  Expect: CR0/CR4 decode, page table walk,\n");
	kprintf("  #PF error code, symbol resolution,\n");
	kprintf("  stack backtrace with symbols.\n");
#else
	kprintf("  EXCEPTION HANDLER TEST: #DE (div0)\n");
	kprintf("========================================\n");
	kprintf("  Expect: GPR dump, CR0/CR4/RFLAGS decode,\n");
	kprintf("  RIP code bytes, symbol resolution,\n");
	kprintf("  stack backtrace with symbols.\n");
#endif
	kprintf("========================================\n");

	exception_test_lv1();

	/* 不会执行到这里 */
	kprintf("  UNEXPECTED: exception did not trap!\n");
	hcf();
}
#endif
