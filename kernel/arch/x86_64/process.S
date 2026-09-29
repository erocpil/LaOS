.intel_syntax noprefix
.global jump_to_user

jump_to_user:
    # rdi = 用户 RIP（其实用不到，因为 iretq 帧里已经有）
    # rsi = 用户 RSP（同上）

    cli                     # 可选，防止中间被打断

    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    # gs 不动，swapgs 会处理

    # 清通用寄存器（可选）
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rbp, rbp
    xor r8,  r8
    xor r9,  r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    # rdi/rsi 保留（如果用户程序需要参数）

    # 栈对齐（重要！）
    mov rax, rsp
    and rax, 15
    jz .aligned
    sub rsp, 8
.aligned:

    # 直接 iretq（rsp 已经指向 create_elf_process 压的 iretq 帧）
    iretq

.section .note.GNU-stack,"",@progbits
