# 注意事项
# 1. .intel_syntax noprefix: 这一行极其重要。它告诉 GCC 汇编器：“我要用 Intel 语法，而且寄存器前面不需要加 % 符号”。
# 2. 注释符号: 在 .S 文件中，建议使用 /* 注释 */ 或 # 注释。
# 3. 寄存器大小: 注意 ax 是 16 位，rdi/rsi 是 64 位。报错中的 operand size mismatch 通常是因为寄存器和立即数大小不匹配导致的。

.intel_syntax noprefix

.global gdt_reload

gdt_reload:
    lgdt [rdi]            /* 加载 GDT 指针 */

    /* 刷新数据段寄存器 */
    mov ax, 0x10          /* 内核数据段偏移 0x10 */
    mov ds, ax
    mov es, ax
# mov fs, ax
# mov gs, ax
    mov ss, ax

	/* 刷新代码段 (CS) */
    /* 64位模式下，我们不需要 pop 返回地址。
       我们直接压入目标 CS 和目标 RIP，然后 retfq。 */
    push 0x08             # 新的 CS (Kernel Code)
    lea rax, [rip + .flush]
    push rax
    retfq                 # 硬件会弹出 rax(RIP) 和 0x08(CS)

.flush:
    ret                   # 此时栈顶是原始的返回地址，正确返回 C 代码

.global tss_load
tss_load:
    mov ax, 0x28    # TSS 在 GDT 中的偏移是 5 * 8 = 40 (0x28)
    ltr ax          # 加载任务寄存器
    ret

.section .note.GNU-stack,"",@progbits
