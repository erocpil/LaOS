; crt0.s (NASM 语法)
[bits 64]

section .text
global _start
extern main    ; C 语言定义的程序入口。

_start:
	; 从初始用户栈中取出 argc 和 argv。
	pop rdi          ; argc。
	mov rsi, rsp     ; argv 指向栈上的参数指针数组。

	; 按 x86_64 ABI 要求，在调用 main() 前将栈指针对齐到 16 字节边界。
	and rsp, -16

	; 调用 C 程序入口。
	call main

	; main() 返回后暂时停机；用户态 exit 系统调用尚未接入此入口。
	hlt
	jmp $

section .note.GNU-stack noalloc noexec nowrite progbits
