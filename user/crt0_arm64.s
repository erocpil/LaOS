/* ARM64 用户态入口（GAS 语法）。
 *
 * 内核通过 ERET 进入 EL0，并设置 x0=argc、sp 指向 argv 数组；当前入口提供的
 * argc 和 argv 均为 0。AAPCS64 要求函数入口处的 sp 按 16 字节对齐。
 */

.section .text
.global _start
.type _start, @function

_start:
	// x0 保存 argc；x1 指向 argv 数组。
	mov  x1, sp          // argv 指向栈顶的参数指针数组。

	// 调用用户程序入口。
	bl   main

	// main() 返回后，通过 svc #1 退出。
	mov  x0, x0          // 返回值已位于 x0。
	svc  #1

	// 若退出系统调用意外返回，则停留在此处。
1:  wfe
	b    1b
	.size _start, . - _start
