/*
 * x86_64 中断描述符表与异常、中断分发。
 *
 * 汇编入口保存处理器现场后调用 idt_handler，由其区分 CPU 异常、外部
 * 中断和 IPI。可恢复的中断处理结束后，汇编返回路径检查 need_resched，
 * 并在允许抢占时进入调度器。键盘和串口输入目前直接在中断上下文处理。
 */

#include <stddef.h>

#include "idt.h"
#include "ist.h"
#include "cpu.h"
#include "tty.h"
#include "printf.h"
#include "timer.h"
#include "debug.h"
#include "hhdm.h"
#include "log.h"
#include "arch_tlb.h"
#include "arch_cpu.h"
#include "serial_arch.h"
#include "export.h"
#include "page_fault.h"

/* e1000 PCI 中断向量（由 main.c 在 PCI 扫描后写入） */
static uint64_t g_e1000_irq_vector;
static void (*g_e1000_irq_handler)(void);

void idt_register_e1000_irq(uint32_t irq_line)
{
	g_e1000_irq_vector = PCI_IRQ_VECTOR_BASE + irq_line;
}

void idt_register_e1000_irq_handler(void (*handler)(void))
{
	g_e1000_irq_handler = handler;
}
EXPORT_SYMBOL(idt_register_e1000_irq_handler);

#include "arch_dispatch.h"
#include "ksym.h"
#include "monitor.h"
#include "ipi.h"
#include "serial_input.h"

// 全局 IDT 表(256个条目)
struct idt_entry idt[256];
struct idt_ptr idt_record;

/* x86 异常名称表 */
static const char *EXCEPTION_NAMES[32] = {
	"#DE Divide Error",		/* 0  */
	"#DB Debug",			/* 1  */
	"NMI Interrupt",		/* 2  */
	"#BP Breakpoint",		/* 3  */
	"#OF Overflow",			/* 4  */
	"#BR BOUND Range Exceeded",	/* 5  */
	"#UD Invalid Opcode",		/* 6  */
	"#NM Device Not Available",	/* 7  */
	"#DF Double Fault",		/* 8  */
	"Coprocessor Segment Overrun",	/* 9  */
	"#TS Invalid TSS",		/* 10 */
	"#NP Segment Not Present",	/* 11 */
	"#SS Stack Fault",		/* 12 */
	"#GP General Protection",	/* 13 */
	"#PF Page Fault",		/* 14 */
	"(Reserved)",			/* 15 */
	"#MF x87 FPU Error",		/* 16 */
	"#AC Alignment Check",		/* 17 */
	"#MC Machine Check",		/* 18 */
	"#XM SIMD Exception",		/* 19 */
	"#VE Virtualization Exception", /* 20 */
	"#CP Control Protection",	/* 21 */
	"(Reserved)",
	"(Reserved)",
	"(Reserved)",
	"(Reserved)",
	"(Reserved)",
	"(Reserved)",
	"(Reserved)",
	"#VC Virtualization Communication", /* 29 */
	"#SX Security Exception",	    /* 30 */
	"(Reserved)",
};

/* CPU 会为 #DF、#TS、#NP、#SS、#GP、#PF、#AC、#CP、#VC 和 #SX 压入错误码。 */
static const uint32_t EXCEPTION_HAS_ERRCODE = 0x60227D00u;

static inline int has_error_code(uint8_t vec)
{
	return (vec < 32) && ((EXCEPTION_HAS_ERRCODE >> vec) & 1);
}

/* 将页表项中本实现关注的标志位解码为可读名称。 */
static void dump_pte_flags(uint64_t entry, int is_leaf)
{
	if (!(entry & 1)) {
		kprintf(" [NOT PRESENT]");
		return;
	}
	kprintf(" [");
	if (entry & (1ULL << 63)) {
		kprintf("NX ");
	}
	if (entry & (1 << 8)) {
		kprintf("G ");
	}
	if (entry & (1 << 7)) {
		kprintf("PS ");
	}
	if (entry & (1 << 6) && is_leaf) {
		kprintf("D ");
	}
	if (entry & (1 << 5)) {
		kprintf("A ");
	}
	if (entry & (1 << 4)) {
		kprintf("PCD ");
	}
	if (entry & (1 << 3)) {
		kprintf("PWT ");
	}
	if (entry & (1 << 2)) {
		kprintf("U ");
	} else {
		kprintf("S ");
	}
	if (entry & (1 << 1)) {
		kprintf("W ");
	} else {
		kprintf("R ");
	}
	kprintf("P]");
}

void debug_dump_paging(uintptr_t virt_addr)
{
	uint64_t cr3 = arch_read_cr3();

	uint64_t pml4_idx = (virt_addr >> 39) & 0x1FF;
	uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FF;
	uint64_t pd_idx = (virt_addr >> 21) & 0x1FF;
	uint64_t pt_idx = (virt_addr >> 12) & 0x1FF;

	kprintf("[Page Table Walk] VA=%p  CR3=%p\n", (void *)virt_addr, (void *)cr3);

	/* 1. PML4 */
	uint64_t *pml4 = phys_to_virt((uint64_t)cr3 & ~0xFFFULL);
	uint64_t pml4e = pml4[pml4_idx];
	kprintf("  PML4[%03lld] = %p", pml4_idx, (void *)pml4e);
	dump_pte_flags(pml4e, 0);
	kprintf(" -> 0x%p\n", (void *)(pml4e & ~0xFFFULL));
	if (!(pml4e & 1)) {
		kprintf("  (walk stopped: PML4 entry not present)\n");
		return;
	}

	/* 2. PDPT */
	uint64_t *pdpt = phys_to_virt((uint64_t)pml4e & ~0xFFFULL);
	uint64_t pdpte = pdpt[pdpt_idx];
	kprintf("  PDPT[%03lld] = %p", pdpt_idx, (void *)pdpte);
	dump_pte_flags(pdpte, (pdpte & 0x80) ? 1 : 0);
	if (pdpte & 0x80) {
		kprintf(" -> Phys 0x%p (1GB page)\n", (void *)(pdpte & ~0x3FFFFFFFULL));
		return;
	}
	kprintf(" -> 0x%p\n", (void *)(pdpte & ~0xFFFULL));
	if (!(pdpte & 1)) {
		kprintf("  (walk stopped: PDPT entry not present)\n");
		return;
	}

	/* 3. PD */
	uint64_t *pd = phys_to_virt((uint64_t)pdpte & ~0xFFFULL);
	uint64_t pde = pd[pd_idx];
	kprintf("    PD[%03lld] = %p", pd_idx, (void *)pde);
	dump_pte_flags(pde, (pde & 0x80) ? 1 : 0);
	if (pde & 0x80) {
		kprintf(" -> Phys 0x%p (2MB page)\n", (void *)(pde & ~0x1FFFFFULL));
		return;
	}
	kprintf(" -> 0x%p\n", (void *)(pde & ~0xFFFULL));
	if (!(pde & 1)) {
		kprintf("    (walk stopped: PD entry not present)\n");
		return;
	}

	/* 4. PT */
	uint64_t *pt = phys_to_virt((uint64_t)pde & ~0xFFFULL);
	uint64_t pte = pt[pt_idx];
	uint64_t phys = pte & ~0xFFFULL;
	kprintf("      PT[%03lld] = %p", pt_idx, (void *)pte);
	dump_pte_flags(pte, 1);
	kprintf(" -> Phys 0x%p\n", (void *)phys);
}

void exception_handler(struct interrupt_frame *frame)
{
	/* 尽早保存 CR2；后续诊断输出若再次触发缺页，CR2 会被新故障地址覆盖。 */
	uint64_t cr0 = arch_read_cr0();
	uint64_t cr2 = arch_read_cr2();
	uint64_t cr3 = arch_read_cr3();
	uint64_t cr4 = arch_read_cr4();

	uint8_t vec = (uint8_t)frame->int_no;
	uint64_t ec = frame->error_code;
	const char *name = (vec < 32) ? EXCEPTION_NAMES[vec] : "(Unknown)";

	/* IDT 生效后、引导线程尚未建立时，current 仍可能为空。 */
	struct thread *cur = cpu_get_ctx()->current;
	const char *proc_name = cur ? cur->name : "(null)";
	int pid = cur ? cur->id : -1;

	kprintf("\n");
	kprintf("================================================\n");
	kprintf("  KERNEL EXCEPTION  vec=%d (%s) CPU %d name %s pid %d\n", vec, name,
		cpu_get_ctx()->id, proc_name, pid);
	kprintf("================================================\n");

	/* 1. 通用寄存器(全量) */
	kprintf("[Registers]\n");
	kprintf("  RAX=%p  RBX=%p  RCX=%p  RDX=%p\n", frame->rax, frame->rbx, frame->rcx,
		frame->rdx);
	kprintf("  RSI=%p  RDI=%p  RBP=%p\n", frame->rsi, frame->rdi, frame->rbp);
	kprintf("  R8 =%p  R9 =%p  R10=%p  R11=%p\n", frame->r8, frame->r9, frame->r10, frame->r11);
	kprintf("  R12=%p  R13=%p  R14=%p  R15=%p\n", frame->r12, frame->r13, frame->r14,
		frame->r15);
	kprintf("  RIP=%p  CS=%x  RFLAGS=%p\n", frame->rip, frame->cs, frame->rflags);
	if (interrupt_frame_has_stack_switch(frame)) {
		kprintf("  interrupted RSP=%p  SS=%x\n", frame->rsp, frame->ss);
	}
	kprintf("  CR0=%p  CR2=%p  CR3=%p  CR4=%p\n", cr0, cr2, cr3, cr4);

	/* 1a. RFLAGS 位解码 */
	{
		uint64_t rf = frame->rflags;
		kprintf("[RFLAGS decode] 0x%x =", rf);
		if (rf & (1 << 9)) {
			kprintf(" IF");
		}
		if (rf & (1 << 8)) {
			kprintf(" TF");
		}
		if (rf & (1 << 10)) {
			kprintf(" DF");
		}
		if (rf & (1 << 18)) {
			kprintf(" AC");
		}
		if (rf & (1 << 21)) {
			kprintf(" ID");
		}
		if (rf & (1 << 11)) {
			kprintf(" OF");
		}
		if (rf & (1 << 7)) {
			kprintf(" SF");
		}
		if (rf & (1 << 6)) {
			kprintf(" ZF");
		}
		if (rf & (1 << 0)) {
			kprintf(" CF");
		}
		if (rf & (1 << 4)) {
			kprintf(" AF");
		}
		if (rf & (1 << 2)) {
			kprintf(" PF");
		}
		uint8_t iopl = (rf >> 12) & 3;
		kprintf(" IOPL=%d", iopl);
		if (rf & (1 << 16)) {
			kprintf(" RF");
		}
		if (rf & (1 << 17)) {
			kprintf(" VM");
		}
		if (rf & (1 << 14)) {
			kprintf(" NT");
		}
		kprintf("\n");
	}

	/* 1b. CR0 位解码(FPU 相关位最关键) */
	kprintf("[CR0 decode] 0x%x:", cr0);
	if (cr0 & (1 << 0)) {
		kprintf(" PE");
	}
	if (cr0 & (1 << 1)) {
		kprintf(" MP");
	}
	if (cr0 & (1 << 2)) {
		kprintf(" EM");
	}
	if (cr0 & (1 << 3)) {
		kprintf(" TS");
	}
	if (cr0 & (1 << 5)) {
		kprintf(" NE");
	}
	if (cr0 & (1 << 16)) {
		kprintf(" WP");
	}
	if (cr0 & (1 << 31)) {
		kprintf(" PG");
	}
	kprintf("\n");

	/* 1b2. CR4 位解码(特性启用位： SSE，SMEP，SMAP 等) */
	kprintf("[CR4 decode] 0x%x:", cr4);
	if (cr4 & (1 << 0)) {
		kprintf(" VME");
	}
	if (cr4 & (1 << 1)) {
		kprintf(" PVI");
	}
	if (cr4 & (1 << 2)) {
		kprintf(" TSD");
	}
	if (cr4 & (1 << 3)) {
		kprintf(" DE");
	}
	if (cr4 & (1 << 4)) {
		kprintf(" PSE");
	}
	if (cr4 & (1 << 5)) {
		kprintf(" PAE");
	}
	if (cr4 & (1 << 6)) {
		kprintf(" MCE");
	}
	if (cr4 & (1 << 7)) {
		kprintf(" PGE");
	}
	if (cr4 & (1 << 8)) {
		kprintf(" PCE");
	}
	if (cr4 & (1 << 9)) {
		kprintf(" OSFXSR");
	}
	if (cr4 & (1 << 10)) {
		kprintf(" OSXMMEXCPT");
	}
	if (cr4 & (1 << 11)) {
		kprintf(" UMIP");
	}
	if (cr4 & (1 << 16)) {
		kprintf(" FSGSBASE");
	}
	if (cr4 & (1 << 17)) {
		kprintf(" PCIDE");
	}
	if (cr4 & (1 << 18)) {
		kprintf(" OSXSAVE");
	}
	if (cr4 & (1 << 20)) {
		kprintf(" SMEP");
	}
	if (cr4 & (1 << 21)) {
		kprintf(" SMAP");
	}
	if (cr4 & (1 << 22)) {
		kprintf(" PKE");
	}
	kprintf("\n");

	/* 2. 解析故障 RIP，并打印其后 16 字节，供指令级诊断。 */
	{
		uint64_t sym_off;
		const char *sym = kallsyms_lookup(frame->rip, &sym_off);
		if (sym) {
			kprintf("[RIP symbol] %s+0x%x\n", sym, sym_off);
		} else {
			kprintf("[RIP symbol] (no symbol found)\n");
		}
	}
	kprintf("[RIP code] ");
	uint8_t *ip_bytes = (uint8_t *)frame->rip;
	for (int i = 0; i < 16; i++) {
		kprintf("%02x ", ip_bytes[i]);
	}
	kprintf("\n");

	/* 3. 仅对 CPU 会压入 error code 的异常打印该字段。 */
	if (has_error_code(vec)) {
		kprintf("[Error Code] 0x%x\n", ec);
	}

	/* 4. 根据异常类型补充解释错误地址或错误码。 */
	switch (vec) {

		case 14: /* #PF Page Fault */
			kprintf("[Page Fault] Faulting VA: %p\n"
				"  Reason : %s\n"
				"  Access : %s\n"
				"  Mode   : %s\n",
				cr2, (ec & 0x1) ? "Protection violation" : "Page not present",
				(ec & 0x2) ? "Write" : "Read",
				(ec & 0x4) ? "User mode" : "Kernel mode");
			if (ec & 0x8) {
				kprintf("  Cause  : Reserved bit set in page table entry\n");
			}
			if (ec & 0x10) {
				kprintf("  Cause  : Instruction fetch (NX violation)\n");
			}
			if (ec & 0x20) {
				kprintf("  Cause  : Protection key violation\n");
			}
			kprintf("Page Fault at %p!\n", cr2);
			debug_dump_paging(cr2);
			break;

		case 13: /* #GP General Protection Fault */
		{
			kprintf("[GPF] ");
			if (ec == 0) {
				kprintf("Error code=0 (non-segment: "
					"non-canonical addr, "
					"misaligned memory access, "
					"privileged instr, "
					"bad MSR write, "
					"or NULL segment load)\n");
			} else {
				/* error code 格式： [15:3]=selector index, [2]=TI, [1]=IDT, [0]=External */
				uint16_t selector = (ec >> 3) & 0x1FFF;
				int is_idt = (ec >> 1) & 1;
				int ext = ec & 1;
				kprintf("Selector index=0x%x  IDT=%d  External=%d\n", selector,
					is_idt, ext);
			}
		} break;

		case 8: /* #DF Double Fault */
			kprintf("[Double Fault] System is in an unrecoverable state.\n");
			break;

		case 0: /* #DE Divide Error */
			kprintf("[Divide Error] DIV/IDIV with zero divisor or quotient "
				"overflow.\n");
			break;

		case 6: /* #UD Invalid Opcode */
			kprintf("[Invalid Opcode] Bad instruction at RIP=%p\n", frame->rip);
			kprintf("  Check: CPUID feature flags for SSE/AVX/etc.\n");
			kprintf("  Instruction bytes: ");
			for (int i = 0; i < 16; i++) {
				kprintf("%02x ", ip_bytes[i]);
			}
			kprintf("\n");
			break;

		case 11: /* #NP Segment Not Present */
		case 12: /* #SS Stack Fault */
		case 10: /* #TS Invalid TSS */
			kprintf("[Segment Fault] Selector=0x%x\n", (ec >> 3) & 0x1FFF);
			break;

		default:
			kprintf("[No additional info for vector %d]\n", vec);
			break;
	}

	/* 5. 基于帧指针进行有界回溯，并尝试解析返回地址符号。 */
	kprintf("[Stack Backtrace]\n");
	{
		uint64_t sym_off;
		const char *sym = kallsyms_lookup(frame->rip, &sym_off);
		if (sym) {
			kprintf("  #0 RIP=%p  <%s+0x%x>\n", frame->rip, sym, sym_off);
		} else {
			kprintf("  #0 RIP=%p  <?+0x0>\n", frame->rip);
		}
	}
	uint64_t *rbp = (uint64_t *)frame->rbp;
	for (int depth = 1; depth < 8 && rbp != NULL; depth++) {
		/* 只检查内核地址范围和对齐；这不能证明该地址一定可读或属于有效帧。 */
		if ((uint64_t)rbp < 0xffff800000000000ULL ||
			(uint64_t)rbp > 0xffffffffffffffffULL || ((uint64_t)rbp & 0x7) != 0) {
			kprintf("  #%d rbp=%p (invalid, stopping)\n", depth, rbp);
			break;
		}
		uint64_t ret_addr = rbp[1];
		uint64_t sym_off;
		const char *sym = kallsyms_lookup(ret_addr, &sym_off);
		if (sym) {
			kprintf("  #%d rbp=%p  ret=%p <%s+0x%x>\n", depth, rbp, ret_addr, sym,
				sym_off);
		} else {
			kprintf("  #%d rbp=%p  ret=%p <?+0x0>\n", depth, rbp, ret_addr);
		}
		rbp = (uint64_t *)rbp[0];
	}

	kprintf("================================================\n");
	kprintf("  System Halted.\n");
	kprintf("================================================\n");

	for (;;) {
		__asm__ volatile("cli; hlt");
	}
}

extern uint64_t isr_stub_table[];

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags)
{
	idt_set_gate_ist(num, base, sel, flags, 0);
}

void idt_set_gate_ist(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist_slot)
{
	idt[num].offset_low = (uint16_t)(base & 0xFFFF);
	idt[num].offset_mid = (uint16_t)((base >> 16) & 0xFFFF);
	idt[num].offset_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);

	idt[num].selector = sel;    // 传入 0x08
	idt[num].ist = ist_slot;    // 0=不切栈；1-7=使用 TSS.ist[ist_slot-1]
	idt[num].type_attr = flags; // 传入 0x8E
	idt[num].reserved = 0;
}

void idt_ap_init(void)
{
	/* BSP 已填好共享 IDT 和 idt_record；每个 AP 仍须在本地执行 lidt。 */
	extern struct idt_ptr idt_record;
	__asm__ volatile("lidt %0" ::"m"(idt_record));
}

void idt_init(void)
{
	idt_record.limit = (sizeof(struct idt_entry) * 256) - 1;
	idt_record.base = (uintptr_t)&idt;

	/* 为 CPU 异常向量 0–31 及当前使用的外部中断向量 32–47 安装入口。 */
	for (int i = 0; i < 48; i++) {
		idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
	}

	/* 将 NMI、双重故障和机器检查切换到各 CPU 的专用 IST 栈，
	 * 避免这些异常继续依赖可能已损坏或耗尽的当前栈。 */
	idt_set_gate_ist(2, isr_stub_table[2], 0x08, 0x8E, IST_SLOT_NMI);
	idt_set_gate_ist(8, isr_stub_table[8], 0x08, 0x8E, IST_SLOT_DF);
	idt_set_gate_ist(18, isr_stub_table[18], 0x08, 0x8E, IST_SLOT_MCE);

	__asm__ volatile("lidt %0" : : "m"(idt_record));

	L_TAG(LOG_BOOT, "IDT loaded.\n");

	/* IPI 向量必须在 APIC 开始投递中断前安装。 */
	idt_register_ipis();

	/* 此函数不开放本地 IRQ；启动流程在 APIC 与调度上下文就绪后开启 IRQ。 */

	L_TAG(LOG_BOOT, "IDT initialized.\n");
}

/* 屏蔽传统 8259 PIC 的全部线路；当前启动路径通过 IOAPIC/LAPIC 投递 IRQ。 */
void pic_disable(void)
{
	/* 先屏蔽从片，再屏蔽主片。 */
	outb(0xA1, 0xFF);
	outb(0x21, 0xFF);
}

/* 传统 8259 PIC 的向量重映射辅助函数。当前启动路径改用 IOAPIC/LAPIC，
 * 不调用本函数，而是在启动时屏蔽 PIC。 */
void pic_remap(void)
{
	/* 进入初始化序列，并配置级联关系。 */
	outb(0x20, 0x11);
	outb(0xA0, 0x11);

	/* 将主、从 PIC 的 IRQ 向量分别映射到 0x20–0x27 和 0x28–0x2F。 */
	outb(0x21, 0x20); // 主片 IRQ 0-7 -> 0x20-0x27
	outb(0xA1, 0x28); // 从片 IRQ 8-15 -> 0x28-0x2F

	/* 声明从片连接到主片的 IRQ2。 */
	outb(0x21, 0x04);
	outb(0xA1, 0x02);

	/* 切换到 8086/88 工作模式。 */
	outb(0x21, 0x01);
	outb(0xA1, 0x01);

	/* 初始化后保持所有 PIC IRQ 屏蔽。 */
	outb(0x21, 0xFF);
	outb(0xA1, 0xFF);
}

/* 键盘修饰键状态，由本地键盘中断路径更新。 */
static int kb_shift = 0;
static int kb_ctrl = 0;
static int kb_alt = 0;
static int kb_caps = 0; /* CapsLock 切换状态 */

/* Set 1 make code 到未按 Shift 时的字符映射。 */
static const char scancode_to_ascii[] = {0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
	'-', '=', '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
	'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '};

/* Set 1 make code 到按下 Shift 时的字符映射。 */
static const char scancode_to_ascii_shifted[] = {0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(',
	')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
	0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
	'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '};

void irq_handler(struct interrupt_frame *frame)
{
	uint64_t irq = frame->int_no;

	if (irq == 32) {
		/* 本地 APIC 定时器中断。 */
		timer_handler(frame);

		cpu_get_ctx()->total_ticks++;
		if (cpu_get_ctx()->current->id == IDLE_PID) {
			cpu_get_ctx()->idle_ticks++;
		}
	} else if (g_e1000_irq_vector && irq == g_e1000_irq_vector) {
		/* 由 PCI IRQ 线路映射得到的 e1000 向量。 */
		if (g_e1000_irq_handler) {
			g_e1000_irq_handler();
		}
	} else if (irq == 33) {
		/* IOAPIC 将键盘线路映射到向量 33。 */
		uint8_t sc = arch_keyboard_read();
		/* 清除 break 位，得到用于查表的 make code。 */
		uint8_t make = sc & ~SC_BREAK;
		int is_break = (sc & SC_BREAK) != 0;

		/* 修饰键只更新状态，不产生字符。 */
		switch (make) {
			case SC_LSHIFT:
			case SC_RSHIFT:
				kb_shift = is_break ? 0 : 1;
				return;

			case SC_LCTRL:
				kb_ctrl = is_break ? 0 : 1;
				return;

			case SC_LALT:
				kb_alt = is_break ? 0 : 1;
				return;

			case SC_CAPSLOCK:
				/* CapsLock 只在按下时翻转,松开不处理 */
				if (!is_break) {
					kb_caps ^= 1;
				}
				return;
		}

		/* 普通键只在按下时生成字符。 */
		if (is_break) {
			return;
		}

		/*
		 * Alt-6/7/8/9/0: 切换到对应 TTY。
		 * 必须先检查 monitor_ready —— 若在 start_monitor() 之前切换，
		 * 当前系统没有任何线程持有 TTY 6-9 的 bitmask(tty_ready() 对所有
		 * 非 monitor 线程返回 false)，导致后续 kprintf 输出只走串口不写 FB，
		 * 屏幕全黑，配合 CONFIG_DEBUG 大量串口输出时表现为系统卡死。
		 *
		 * kb_alt 由修饰键的 make/break 统一维护，Alt-number 分支不做清零：
		 * 按住 Alt 不松手可连续切多个 TTY。tty_switch() 内部忽略重复切换。
		 */
		if (kb_alt && make == 0x0A) { // 0x0A = '9'
			if (!monitor_ready) {
				return;
			}
			tty_switch(TTY_MONITOR);
		} else if (kb_alt && make == 0x0B) { // 0x0B = '0'
			tty_switch(0);
		} else if (kb_alt && make == 0x09) { // 8
			if (!monitor_ready) {
				return;
			}
			tty_switch(8);
		} else if (kb_alt && make == 0x08) { // 7
			if (!monitor_ready) {
				return;
			}
			tty_switch(7);
		} else if (kb_alt && make == 0x07) { // 6
			if (!monitor_ready) {
				return;
			}
			tty_switch(6);
		}

		/* 忽略映射表范围之外的扫描码。 */
		if (make >= sizeof(scancode_to_ascii)) {
			return;
		}

		char c = kb_shift ? scancode_to_ascii_shifted[make] : scancode_to_ascii[make];
		if (c == 0) {
			return;
		}

		/* CapsLock 只反转字母大小写，不改变数字和标点。 */
		if (kb_caps) {
			if (c >= 'a' && c <= 'z') {
				c -= 32;
			} else if (c >= 'A' && c <= 'Z') {
				c += 32;
			}
		}

		/* 将 Ctrl+字母转换为 ASCII 控制字符；当前路径尚未将其送入 TTY。 */
		if (kb_ctrl && c >= 'a' && c <= 'z') {
			/* Ctrl+A 到 Ctrl+Z 对应 0x01 到 0x1A。 */
			c -= ('a' - 1);
			return;
		}

		if ('c' == c) {
			fb_clear_screen(0);
		}
	} else if (irq == 36) {
		/* COM1 的 IRQ4 由 IOAPIC 映射到向量 36。 */
		uint8_t lsr = arch_inb(0x3FD);
		if (lsr & 1) {
			serial_input_process(arch_serial_read());
		}
	}
}

/* 扫描码表使用 make code；break code 在同一 make code 上置位 0x80，
 * 因而按键按下和松开共用一张映射表。Shift 表先生成字母大写，CapsLock
 * 再反转字母大小写，所以 Shift 与 CapsLock 同时按下时得到小写字母。
 * Ctrl+字母会计算为 ASCII 控制字符，但当前分支直接返回，尚未接入 TTY
 * 输入队列。Alt 目前用于切换 TTY；其他组合键尚未实现。 */

void debug_print_iretq_frame5(
	uint64_t rip, uint64_t cs, uint64_t rflags, uint64_t user_rsp, uint64_t ss)
{
	kprintf("[iretq] RIP=%p CS=%p RFLAGS=%p USP=%p SS=%p => %s\n", rip, cs, rflags, user_rsp,
		ss, (cs & 3) ? "USER" : "!!! KERNEL !!!");
}

/* 直接向 QEMU debugcon（I/O 端口 0xE9）输出，不经过 kprintf。 */
static void putx(uint64_t val)
{
	for (int i = 60; i >= 0; i -= 4) {
		uint8_t nibble = (val >> i) & 0xF;
		uint8_t c = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
		__asm__ volatile("outb %0, $0xe9" ::"a"(c));
	}
	__asm__ volatile("outb %0, $0xe9" ::"a"((uint8_t)'\n'));
}

void dump_iretq_stack_raw(uint64_t *rsp)
{
	/* 直接写端口，避免调试输出依赖锁和缓冲区。 */
	const char *msg = "RAW:";
	for (int i = 0; msg[i]; i++) {
		__asm__ volatile("outb %0, $0xe9" ::"a"((uint8_t)msg[i]));
	}
	putx(rsp[0]); // RIP
	putx(rsp[1]); // CS
	putx(rsp[2]); // RFLAGS
	putx(rsp[3]); // RSP
	putx(rsp[4]); // SS
}

void debug_print_cs_before_swapgs(uint64_t cs)
{
	kprintf("[ret_from_intr] CS at [rsp+144] = %p\n", cs);
}

void idt_handler(struct interrupt_frame *frame)
{
	/* IPI 各有独立入口；先完成 LAPIC 应答，再执行相应处理。 */
	if (frame->int_no == IPI_VECTOR_TLB) {
		EOI();

		/* 在当前 CPU 上刷新本地 TLB。 */
		arch_tlb_flush_all();
		L("CPU %d: %s %ld TLB Flushed via IPI", cpu_get_ctx()->id,
			cpu_get_ctx()->current->name, cpu_get_ctx()->current->id);

				/* 通知已注册的观测回调，例如自测和诊断模块。 */
		ipi_tlb_invoke_callbacks();

		return;
	}

	if (frame->int_no == IPI_VECTOR_SELFTEST) {
		EOI();
		ipi_selftest_invoke_callbacks();
		return;
	}

	if (frame->int_no == IPI_VECTOR_RESCHEDULE) {
		/* 发送方先以 release 顺序设置 need_resched；common_stub 在本处理
		 * 函数返回后重新检查该标志，再决定是否进入调度器。 */
		EOI();
		ipi_reschedule_ack();
		return;
	}

	if (3 == frame->int_no) {
		panic("int no 3");
		hcf();
	}

	/* CPU 异常向量范围为 0–31。 */
	if (frame->int_no < 32) {
		/* #PF 可按需补页或终止故障用户线程；无法恢复的内核故障会 panic。 */
		if (frame->int_no == 14) {
			page_fault_handler(frame);
		} else {
			exception_handler(frame);
		}
		return; /* CPU 异常不是 LAPIC 外部中断，不发送 EOI。 */
	}

	/* LAPIC spurious vector 不要求发送 EOI。 */
	if (frame->int_no == 0xFF) {
		return;
	}

	/* 当前已安装的外部 IRQ 门位于 32–47；IPI 向量在前面单独处理。 */
	if (frame->int_no >= 32 && frame->int_no <= 47) {
		/* 在调用设备处理函数前应答 LAPIC；抢占检查由汇编返回路径执行。 */
		EOI();

		/* 分发定时器、键盘、串口或已注册的 PCI IRQ。 */
		irq_handler(frame);
		return;
	}

	/* 用户系统调用使用 SYSCALL 指令；0x80 向量不承担系统调用分发。 */
	if (frame->int_no == 0x80) {
		return;
	}

	/* 对未知向量应答一次，以清除其可能留下的 LAPIC in-service 状态。 */
	L("Unknown Interrupt: %ld", frame->int_no);

	EOI();
}

/* 汇编入口由 idt_stubs.S 提供。 */
extern void irq0();
extern void irq1();
extern void ipi_tlb_entry();
extern void ipi_selftest_entry();
extern void ipi_reschedule_entry();

/*
 * idt_register_ipis() - 注册 TLB、selftest 和 reschedule IPI 中断门。
 *
 * idt_init() 已为向量 0–47 安装通用 stub；本函数补充高位 IPI 向量。
 * 向量号由 ipi.h 定义，处理函数在 idt_handler() 中按向量分派。
 */
void idt_register_ipis()
{
	/* 内核态中断门：TLB shootdown。 */
	idt_set_gate(IPI_VECTOR_TLB, (uintptr_t)ipi_tlb_entry, 0x08, 0x8E);
	/* 内核态中断门：测试回调。 */
	idt_set_gate(IPI_VECTOR_SELFTEST, (uintptr_t)ipi_selftest_entry, 0x08, 0x8E);
	/* 内核态中断门：远端 CPU reschedule 请求。 */
	idt_set_gate(IPI_VECTOR_RESCHEDULE, (uintptr_t)ipi_reschedule_entry, 0x08, 0x8E);
}
