/* Reload the GDT, refresh CS, and load the task register from C-callable
	* entry points. This file uses Intel syntax. */

.intel_syntax noprefix

.global gdt_reload

gdt_reload:
	lgdt [rdi]            /* 加载 GDT 指针 */

	/* 刷新数据段寄存器 */
	mov ax, 0x10          /* 内核数据段偏移 0x10 */
	mov ds, ax
	mov es, ax
	/* FS and GS bases are managed separately through MSRs. */
	mov ss, ax

	/* 刷新代码段 (CS) */
	/* Reload CS with a far return, then resume at the local label. */
	push 0x08             /* Kernel code selector. */
	lea rax, [rip + .flush]
	push rax
	retfq                 /* Pops the target RIP and CS. */

.flush:
	ret                   /* Return to the C caller. */

.global tss_load
tss_load:
	mov ax, 0x28    /* TSS descriptor selector at GDT entries 5–6. */
	ltr ax          /* Load the task register. */
	ret

.section .note.GNU-stack,"",@progbits
