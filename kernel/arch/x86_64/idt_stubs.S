#include "asm_offsets_gas.h"

# x86_64 异常与 IRQ 入口桩。无错误码的向量由桩补入占位值，
# 使各入口具有统一的向量号、错误码和寄存器保存布局。
.intel_syntax noprefix

# 保存和恢复顺序必须与 struct interrupt_frame 的字段布局一致。
.macro PUSH_ALL
	push r15;
	push r14;
	push r13;
	push r12;
	push r11;
	push r10;
	push r9;
	push r8;
	push rdi;
	push rsi;
	push rbp;
	push rdx;
	push rcx;
	push rbx;
	push rax;
.endm

.macro POP_ALL
	pop rax;
	pop rbx;
	pop rcx;
	pop rdx;
	pop rbp;
	pop rsi;
	pop rdi;
	pop r8;
	pop r9;
	pop r10;
	pop r11;
	pop r12;
	pop r13;
	pop r14;
	pop r15;
.endm

# 异常入口宏。
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
	push 0          # 压入伪错误代码
	push \num       # 压入中断向量号
	jmp common_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
	# 错误代码已被 CPU 压入
	push \num       # 压入中断向量号
	jmp common_stub
.endm

# 外部中断入口宏。
.macro IRQ_STUB num vector
.global irq\num
irq\num:
	push 0          # IRQ 没有错误代码，补 0
	push \vector    # 压入重映射后的向量号 (32, 33...)
	jmp common_stub
.endm

# CPU 异常向量 0–31。
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_ERRCODE   29
ISR_ERRCODE   30
ISR_NOERRCODE 31

# 当前使用的外部中断向量 32–47。
IRQ_STUB 0 32
IRQ_STUB 1 33
IRQ_STUB 2 34
IRQ_STUB 3 35
IRQ_STUB 4 36
IRQ_STUB 5 37
IRQ_STUB 6 38
IRQ_STUB 7 39
IRQ_STUB 8 40
IRQ_STUB 9 41
IRQ_STUB 10 42
IRQ_STUB 11 43
IRQ_STUB 12 44
IRQ_STUB 13 45
IRQ_STUB 14 46
IRQ_STUB 15 47

# 统一入口：保存现场、调用 C 分发器，并按原栈帧恢复现场。
.extern idt_handler
.extern check_need_schedule
.extern __schedule_irq

# Regular IRQ/IPI vectors currently keep their frames on the interrupted
# kernel stack.  Do not switch to a shared per-CPU stack here: when
# __schedule_irq() calls switch_to(), the suspended interrupt continuation
# must remain owned by the interrupted thread.  A future implementation
# needs an explicit per-thread continuation design, not an RSP range check.

common_stub:

	test byte ptr [rsp + INTERRUPT_FRAME_CS_END], 3
	jz .skip_swap_in
	swapgs

.skip_swap_in:
	PUSH_ALL
	mov rdi, rsp
	call idt_handler

ret_from_intr:
	call check_need_schedule
	test eax, eax
	jz 1f
	call __schedule_irq
	jmp ret_from_intr

1:
	test byte ptr [rsp + INTERRUPT_FRAME_CS], 3
	jz .restore
	swapgs

.restore:
	POP_ALL
	add rsp, 16
	iretq



.extern ipi_handler
.global ipi_tlb_entry
ipi_tlb_entry:
	push 0          # 伪错误代码，保证 interrupt_frame 结构对齐
	push 253        # 压入 IPI 向量号 (0xFD)
	jmp common_stub # 进入通用的上下文保存和分发流程

.global ipi_selftest_entry
ipi_selftest_entry:
	push 0          # 伪错误代码
	push 252        # 压入 IPI 向量号 (0xFC)
	jmp common_stub

.global ipi_reschedule_entry
ipi_reschedule_entry:
	push 0          # 伪错误代码
	push 251        # 压入 IPI 向量号 (0xFB)
	jmp common_stub



# 导出供 C 代码索引的入口地址表。
.section .rodata
.global isr_stub_table
isr_stub_table:
	# 异常 (0-31)
	.quad isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
	.quad isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
	.quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
	.quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
	# IRQ (32-47)
	.quad irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
	.quad irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

.section .note.GNU-stack,"",@progbits
