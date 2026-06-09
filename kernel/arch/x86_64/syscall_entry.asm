%include "asm_offsets_nasm.inc"
[BITS 64]
global syscall_entry
extern syscall_handler

syscall_entry:
    swapgs          ; 切换到内核 GS (指向 Per-CPU 数据)

    ; 假设我们在 GS 偏移 0 处保存了内核栈顶,偏移 8 处保存了临时用户 RSP
    mov [gs:CPU_CTX_USER_RSP ], rsp
	; 这里如果使用ctx->kernel_stack,只能是单用户进程
    ; mov rsp, [gs:0]
	; 第一步:获取当前线程结构体指针
    ; 假设 current 成员在 cpu_context 偏移 16 字节处
    mov rsp, [gs:CPU_CTX_CURRENT]                ; rsp 现在指向 struct thread *t

    ; 第二步:切换到该线程专属的内核栈
    ; 假设 kernel_stack 成员在 struct thread 中的偏移是 24 字节
    mov rsp, [rsp + THREAD_KSTACK]             ; rsp = t->kernel_stack

    ; 现在,RSP 已经安全地指向了 user1 专属的栈空间
    ; 即使在此处调用 schedule(),切换回来时这个栈的内容也是原封不动的

    ; 第三步:压入中断栈帧(为了让 iretq/sysret 能正确返回)


    ; 构造类似中断的栈帧,方便统一处理.
    ; struct trap_frame 顺序 (低 -> 高 = 后压 -> 先压):
    ;   ... rip, cs, rflags, rsp, ss
    ; 因此第一个 push 落在最高地址 ss 槽.原版 line 28/31 SEL 写反,
    ; ss 槽压了 USER_CS,cs 槽压了 USER_SS:因为 sysret 不读栈,syscall.c
    ; 也未读 regs->cs/ss,没现场触发;但任何未来用 regs->cs 判态,或改走
    ; iretq 返回的尝试都会立刻翻车.
    push USER_SS_SEL; ss 槽 (0x1b)
    push qword [gs:CPU_CTX_USER_RSP] ; 用户 RSP
    push r11        ; 保存用户 RFLAGS
    push USER_CS_SEL; cs 槽 (0x23)
    push rcx        ; 保存用户 RIP

    ; 保存通用寄存器 (由 C 调用约定决定哪些需要 push)
    push rbp
    push rbx
    push rdi
    push rsi
    push rdx
    push r10        ; syscall 使用 r10 传递原本属于 rcx 的参数
    push r8
    push r9
    push rax        ; 系统调用号

    ; 将栈指针作为参数传给 C 处理函数
    mov rdi, rsp
	call syscall_handler

    ; 恢复寄存器
    pop rax
	pop r9
	pop r8
	pop r10
	pop rdx
	pop rsi
	pop rdi
	pop rbx
	pop rbp

    pop rcx         ; 恢复用户 RIP
    add rsp, 8      ; 跳过 CS
    pop r11         ; 恢复 RFLAGS
    pop qword [gs:CPU_CTX_USER_RSP] ; 暂存用户 RSP
    add rsp, 8      ; 跳过 SS

    mov rsp, [gs:CPU_CTX_USER_RSP] ; 真正切回用户栈

    swapgs
    o64 sysret
	;

section .note.GNU-stack noalloc noexec nowrite progbits
