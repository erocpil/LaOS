%include "asm_offsets_nasm.inc"
[BITS 64]
global syscall_entry
extern syscall_handler

syscall_entry:
	swapgs          ; 切换到内核 GS (指向 Per-CPU 数据)

	; 保存用户 RSP 到每 CPU 上下文。
	mov [gs:CPU_CTX_USER_RSP ], rsp
	; 取得当前线程，并切换到其专属内核栈。
	mov rsp, [gs:CPU_CTX_CURRENT]                ; rsp 现在指向 struct thread *t

	mov rsp, [rsp + THREAD_KSTACK]             ; rsp = t->kernel_stack

	; 构造 trap_frame 的用户态返回状态。SYSCALL 将返回 RIP/RFLAGS 分别放在
	; RCX/R11 中，不自动保存用户 RSP 或段选择子。
	push USER_SS_SEL; ss 槽 (0x1b)
	push qword [gs:CPU_CTX_USER_RSP] ; 用户 RSP
	push r11        ; 保存用户 RFLAGS
	push USER_CS_SEL; cs 槽 (0x23)
	push rcx        ; 保存用户 RIP

	; 保存通用寄存器（完整 syscall frame 所需）
	push rbp
	push rbx
	push rdi
	push rsi
	push rdx
	push r10        ; syscall 使用 r10 传递原本属于 rcx 的参数
	push r8
	push r9
	push r15
	push r14
	push r13
	push r12
	push rax        ; 系统调用号

	; 将栈指针作为参数传给 C 处理函数
	mov rdi, rsp
	call syscall_handler

	; 恢复寄存器
	pop rax
	pop r12
	pop r13
	pop r14
	pop r15
	pop r9
	pop r8
	pop r10
	pop rdx
	pop rsi
	pop rdi
	pop rbx
	pop rbp

	; TODO(SYSRET 返回校验): 在仍使用内核栈和内核 GS 时，检查最终返回 RIP
	; 的规范地址编码及用户地址范围，并核对 SYSRET 所需的选择子和 RFLAGS。
	; Intel 上非规范 RCX 可使 SYSRET 在 Ring 0 触发 #GP；不能等到恢复用户
	; RSP 和 swapgs 后再检查。非法状态应拒绝返回，合法但不适用 SYSRET 的
	; 状态需要受保护的 IRETQ 路径。入口/出口 RIP 比较仅可作为调试辅助手段。
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
