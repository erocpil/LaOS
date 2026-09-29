#ifndef __IDT_H__
#define __IDT_H__

/*
 * idt.h - IDT 门描述符结构与 idt_set_gate API
 */

#include <stdint.h>

#include "lapic.h"

/* -- 扫描码常量 -- */
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_LCTRL    0x1D
#define SC_LALT     0x38
#define SC_CAPSLOCK 0x3A
#define SC_BREAK    0x80 /* break code = make code | 0x80 */

// IDT 条目结构
struct idt_entry {
	uint16_t offset_low;  // 偏移量 0-15
	uint16_t selector;    // 段选择子 (使用刚才 GDT 里的 0x08)
	uint8_t  ist;         // 中断栈表索引 (填 0)
	uint8_t  type_attr;   // 类型和属性 (填 0x8E: 存在 + Ring 0 + 中断门)
	uint16_t offset_mid;  // 偏移量 16-31
	uint32_t offset_high; // 偏移量 32-63
	uint32_t reserved;    // 保留
} __attribute__((packed));

// lidt 指令需要的结构
struct idt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

/*
 * interrupt_frame - ISR 压栈顺序与 C 结构体严格对应
 *
 * 压栈顺序(PUSH_ALL + CPU):
 *   PUSH_ALL: rax, rbx, rcx, rdx, rbp, rsi, rdi, r8-r15
 *   stub:     int_no, error_code(0 for no-error vectors)
 *   CPU:      rip, cs, rflags, rsp, ss
 *
 * frame = RSP 指向最后压入的 rax，因此结构体从 rax 开始。
 */
struct interrupt_frame {
	uint64_t rax, rbx, rcx, rdx, rbp, rsi, rdi;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t int_no, error_code;
	uint64_t rip, cs, rflags, rsp, ss; /* CPU 自动压入 */
};

#define EOI() lapic_eoi()

void idt_init(void);
void idt_ap_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);
void idt_set_gate_ist(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist_slot);
void pic_disable(void);
void idt_register_ipis(void);
void idt_activete_pic_remap(void);
void idt_register_e1000_irq(uint32_t irq_line);
void idt_register_e1000_irq_handler(void (*handler)(void));

#define DUMP_STACK(frame) \
	do { \
		struct cpu_context *ctx = get_cpu_ctx(); \
		kprintf("current %p pid %d\n", ctx->current_thread, ctx->current_thread->id); \
		kprintf("r15          %p\n", frame->r15); \
		kprintf("r14          %p\n", frame->r14); \
		kprintf("r13          %p\n", frame->r13); \
		kprintf("r12          %p\n", frame->r12); \
		kprintf("r11          %p\n", frame->r11); \
		kprintf("r10          %p\n", frame->r10); \
		kprintf("r9           %p\n", frame->r9); \
		kprintf("r8           %p\n", frame->r8); \
		kprintf("rdi          %p\n", frame->rdi); \
		kprintf("rsi          %p\n", frame->rsi); \
		kprintf("rbp          %p\n", frame->rbp); \
		kprintf("rdx          %p\n", frame->rdx); \
		kprintf("rcx          %p\n", frame->rcx); \
		kprintf("rbx          %p\n", frame->rbx); \
		kprintf("rax          %p\n", frame->rax); \
		kprintf("int_no       %p\n", frame->int_no); \
		kprintf("error_code   %p\n", frame->error_code); \
		kprintf("rip          %p\n", frame->rip); \
		kprintf("cs           %p\n", frame->cs); \
		kprintf("rflags       %p\n", frame->rflags); \
		kprintf("rsp          %p\n", frame->rsp); \
		kprintf("ss           %p\n", frame->ss); \
	} while (0);

#endif
