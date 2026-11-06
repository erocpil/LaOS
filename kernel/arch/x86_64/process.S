.intel_syntax noprefix
.global jump_to_user

jump_to_user:
	# rdi/rsi 保留为接口参数；用户 RIP 和 RSP 已包含在 iretq 帧中。

	cli                     # 在切换 GS 基址前屏蔽本地可屏蔽中断。

	mov ax, 0x23
	mov ds, ax
	mov es, ax
	mov fs, ax
	# GS 基址由后续 swapgs 切换。

	# 清理用户态可见的通用寄存器。
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
	# 保留 rdi/rsi，供用户程序接收启动参数。

	# 按 iretq 帧约定调整内核栈位置。
	mov rax, rsp
	and rax, 15
	jz .aligned
	sub rsp, 8
.aligned:

	# rsp 指向调用方预先构造的 iretq 帧。
	iretq

.section .note.GNU-stack,"",@progbits
