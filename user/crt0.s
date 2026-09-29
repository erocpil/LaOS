; crt0.s (NASM 语法)
[bits 64]

section .text
global _start
extern main    ; 声明外部 C 函数

_start:
    ; 1. 获取 argc 和 argv
    pop rdi          ; argc
    mov rsi, rsp     ; argv (指向栈上剩余的指针数组)

    ; 2. 栈对齐
    ; 注意：x86_64 规定 call 之前栈必须 16 字节对齐
    ; pop 之后栈已经变动，此处强行对齐
    and rsp, -16

    ; 3. 调用 C 的 main
    call main

    ; 4. main 返回后进入死循环 (以后可以改成 exit 系统调用)
    hlt
    jmp $

section .note.GNU-stack noalloc noexec nowrite progbits
