/*
 * idt.c - IDT 初始化，异常处理，键盘中断，IPI 路由
 *
 * 汇编 trampoline -> idt_handler 分发(异常->exception_handler,
 * IRQ->irq_handler,IPI->TLB 刷新等).
 * 键盘驱动嵌入在键盘中断路径中(esc/Alt+9/0 tty 切换).
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

/* idt.c - exception_handler 完善版 */

/* x86 异常名称表 */
static const char *EXCEPTION_NAMES[32] = {
	"#DE Divide Error",              /* 0  */
	"#DB Debug",                     /* 1  */
	"NMI Interrupt",                 /* 2  */
	"#BP Breakpoint",                /* 3  */
	"#OF Overflow",                  /* 4  */
	"#BR BOUND Range Exceeded",      /* 5  */
	"#UD Invalid Opcode",            /* 6  */
	"#NM Device Not Available",      /* 7  */
	"#DF Double Fault",              /* 8  */
	"Coprocessor Segment Overrun",   /* 9  */
	"#TS Invalid TSS",               /* 10 */
	"#NP Segment Not Present",       /* 11 */
	"#SS Stack Fault",               /* 12 */
	"#GP General Protection",        /* 13 */
	"#PF Page Fault",                /* 14 */
	"(Reserved)",                    /* 15 */
	"#MF x87 FPU Error",             /* 16 */
	"#AC Alignment Check",           /* 17 */
	"#MC Machine Check",             /* 18 */
	"#XM SIMD Exception",            /* 19 */
	"#VE Virtualization Exception",  /* 20 */
	"#CP Control Protection",        /* 21 */
	"(Reserved)", "(Reserved)", "(Reserved)",
	"(Reserved)", "(Reserved)", "(Reserved)",
	"(Reserved)", "(Reserved)", "(Reserved)",
	"(Reserved)",
};

/* error_code 是否有意义(CPU 会压栈 error code 的异常) */
static const uint32_t EXCEPTION_HAS_ERRCODE = 0x00027D00u;
/* bit mask: 8,10,11,12,13,14,17,21 有 error code */

static inline int has_error_code(uint8_t vec)
{
	return (vec < 32) && ((EXCEPTION_HAS_ERRCODE >> vec) & 1);
}

/** dump_pte_flags() - 解码页表项的标志位，输出命名位字符串 */
static void dump_pte_flags(uint64_t entry, int is_leaf)
{
	if (!(entry & 1)) {
		kprintf(" [NOT PRESENT]");
		return;
	}
	kprintf(" [");
	if (entry & (1ULL<<63)) {
		kprintf("NX ");
	}
	if (entry & (1<<8)) {
		kprintf("G ");
	}
	if (entry & (1<<7)) {
		kprintf("PS ");
	}
	if (entry & (1<<6) && is_leaf) {
		kprintf("D ");
	}
	if (entry & (1<<5)) {
		kprintf("A ");
	}
	if (entry & (1<<4)) {
		kprintf("PCD ");
	}
	if (entry & (1<<3)) {
		kprintf("PWT ");
	}
	if (entry & (1<<2)) {
		kprintf("U ");
	} else {
		kprintf("S ");
	}
	if (entry & (1<<1)) {
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

	kprintf("[Page Table Walk] VA=%p  CR3=%p\n", (void*)virt_addr, (void*)cr3);

	/* 1. PML4 */
	uint64_t *pml4 = phys_to_virt((uint64_t)cr3 & ~0xFFFULL);
	uint64_t pml4e = pml4[pml4_idx];
	kprintf("  PML4[%03lld] = %p", pml4_idx, (void*)pml4e);
	dump_pte_flags(pml4e, 0);
	kprintf(" -> 0x%p\n", (void*)(pml4e & ~0xFFFULL));
	if (!(pml4e & 1)) {
		kprintf("  (walk stopped: PML4 entry not present)\n");
		return;
	}

	/* 2. PDPT */
	uint64_t *pdpt = phys_to_virt((uint64_t)pml4e & ~0xFFFULL);
	uint64_t pdpte = pdpt[pdpt_idx];
	kprintf("  PDPT[%03lld] = %p", pdpt_idx, (void*)pdpte);
	dump_pte_flags(pdpte, (pdpte & 0x80) ? 1 : 0);
	if (pdpte & 0x80) {
		kprintf(" -> Phys 0x%p (1GB page)\n", (void*)(pdpte & ~0x3FFFFFFFULL));
		return;
	}
	kprintf(" -> 0x%p\n", (void*)(pdpte & ~0xFFFULL));
	if (!(pdpte & 1)) {
		kprintf("  (walk stopped: PDPT entry not present)\n");
		return;
	}

	/* 3. PD */
	uint64_t *pd = phys_to_virt((uint64_t)pdpte & ~0xFFFULL);
	uint64_t pde = pd[pd_idx];
	kprintf("    PD[%03lld] = %p", pd_idx, (void*)pde);
	dump_pte_flags(pde, (pde & 0x80) ? 1 : 0);
	if (pde & 0x80) {
		kprintf(" -> Phys 0x%p (2MB page)\n", (void*)(pde & ~0x1FFFFFULL));
		return;
	}
	kprintf(" -> 0x%p\n", (void*)(pde & ~0xFFFULL));
	if (!(pde & 1)) {
		kprintf("    (walk stopped: PD entry not present)\n");
		return;
	}

	/* 4. PT */
	uint64_t *pt = phys_to_virt((uint64_t)pde & ~0xFFFULL);
	uint64_t pte = pt[pt_idx];
	uint64_t phys = pte & ~0xFFFULL;
	kprintf("      PT[%03lld] = %p", pt_idx, (void*)pte);
	dump_pte_flags(pte, 1);
	kprintf(" -> Phys 0x%p\n", (void*)phys);
}

void exception_handler(struct interrupt_frame *frame)
{
	/* CR2 必须在最开始读，越早越好，避免后续代码(如 kprintf 内部)触发缺页把它覆盖 */
	uint64_t cr0 = arch_read_cr0();
	uint64_t cr2 = arch_read_cr2();
	uint64_t cr3 = arch_read_cr3();
	uint64_t cr4 = arch_read_cr4();

	uint8_t vec = (uint8_t)frame->int_no;
	uint64_t ec = frame->error_code;
	const char *name = (vec < 32) ? EXCEPTION_NAMES[vec] : "(Unknown)";

	/* 早期异常可能在 current 初始化之前，做 NULL guard */
	struct thread *cur = cpu_get_ctx()->current;
	const char *proc_name = cur ? cur->name : "(null)";
	int pid = cur ? cur->id : -1;

	kprintf("\n");
	kprintf("================================================\n");
	kprintf("  KERNEL EXCEPTION  vec=%d (%s) CPU %d name %s pid %d\n",
			vec, name, cpu_get_ctx()->id, proc_name, pid);
	kprintf("================================================\n");

	/* 1. 通用寄存器(全量) */
	kprintf("[Registers]\n");
	kprintf("  RAX=%p  RBX=%p  RCX=%p  RDX=%p\n",
			frame->rax, frame->rbx, frame->rcx, frame->rdx);
	kprintf("  RSI=%p  RDI=%p  RBP=%p  RSP=%p\n",
			frame->rsi, frame->rdi, frame->rbp, frame->rsp);
	kprintf("  R8 =%p  R9 =%p  R10=%p  R11=%p\n",
			frame->r8,  frame->r9,  frame->r10, frame->r11);
	kprintf("  R12=%p  R13=%p  R14=%p  R15=%p\n",
			frame->r12, frame->r13, frame->r14, frame->r15);
	kprintf("  RIP=%p  CS=%x  RFLAGS=%p  SS=%x\n",
			frame->rip, frame->cs, frame->rflags, frame->ss);
	kprintf("  CR0=%p  CR2=%p  CR3=%p  CR4=%p\n",
			cr0, cr2, cr3, cr4);

	/* 1a. RFLAGS 位解码 */
	{
		uint64_t rf = frame->rflags;
		kprintf("[RFLAGS decode] 0x%x =", rf);
		if (rf & (1<<9)) {
			kprintf(" IF");
		}
		if (rf & (1<<8)) {
			kprintf(" TF");
		}
		if (rf & (1<<10)) {
			kprintf(" DF");
		}
		if (rf & (1<<18)) {
			kprintf(" AC");
		}
		if (rf & (1<<21)) {
			kprintf(" ID");
		}
		if (rf & (1<<11)) {
			kprintf(" OF");
		}
		if (rf & (1<<7)) {
			kprintf(" SF");
		}
		if (rf & (1<<6)) {
			kprintf(" ZF");
		}
		if (rf & (1<<0)) {
			kprintf(" CF");
		}
		if (rf & (1<<4)) {
			kprintf(" AF");
		}
		if (rf & (1<<2)) {
			kprintf(" PF");
		}
		uint8_t iopl = (rf >> 12) & 3;
		kprintf(" IOPL=%d", iopl);
		if (rf & (1<<16)) {
			kprintf(" RF");
		}
		if (rf & (1<<17)) {
			kprintf(" VM");
		}
		if (rf & (1<<14)) {
			kprintf(" NT");
		}
		kprintf("\n");
	}

	/* 1b. CR0 位解码(FPU 相关位最关键) */
	kprintf("[CR0 decode] 0x%x:", cr0);
	if (cr0 & (1<<0)) {
		kprintf(" PE");
	}
	if (cr0 & (1<<1)) {
		kprintf(" MP");
	}
	if (cr0 & (1<<2)) {
		kprintf(" EM");
	}
	if (cr0 & (1<<3)) {
		kprintf(" TS");
	}
	if (cr0 & (1<<5)) {
		kprintf(" NE");
	}
	if (cr0 & (1<<16)) {
		kprintf(" WP");
	}
	if (cr0 & (1<<31)) {
		kprintf(" PG");
	}
	kprintf("\n");

	/* 1b2. CR4 位解码(特性启用位： SSE，SMEP，SMAP 等) */
	kprintf("[CR4 decode] 0x%x:", cr4);
	if (cr4 & (1<<0)) {
		kprintf(" VME");
	}
	if (cr4 & (1<<1)) {
		kprintf(" PVI");
	}
	if (cr4 & (1<<2)) {
		kprintf(" TSD");
	}
	if (cr4 & (1<<3)) {
		kprintf(" DE");
	}
	if (cr4 & (1<<4)) {
		kprintf(" PSE");
	}
	if (cr4 & (1<<5)) {
		kprintf(" PAE");
	}
	if (cr4 & (1<<6)) {
		kprintf(" MCE");
	}
	if (cr4 & (1<<7)) {
		kprintf(" PGE");
	}
	if (cr4 & (1<<8)) {
		kprintf(" PCE");
	}
	if (cr4 & (1<<9)) {
		kprintf(" OSFXSR");
	}
	if (cr4 & (1<<10)) {
		kprintf(" OSXMMEXCPT");
	}
	if (cr4 & (1<<11)) {
		kprintf(" UMIP");
	}
	if (cr4 & (1<<16)) {
		kprintf(" FSGSBASE");
	}
	if (cr4 & (1<<17)) {
		kprintf(" PCIDE");
	}
	if (cr4 & (1<<18)) {
		kprintf(" OSXSAVE");
	}
	if (cr4 & (1<<20)) {
		kprintf(" SMEP");
	}
	if (cr4 & (1<<21)) {
		kprintf(" SMAP");
	}
	if (cr4 & (1<<22)) {
		kprintf(" PKE");
	}
	kprintf("\n");

	/* 1. RIP 符号解析，指令字节 */
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

	/* 2. Error Code(只在有意义时打印) */
	if (has_error_code(vec)) {
		kprintf("[Error Code] 0x%x\n", ec);
	}

	/* 3. 异常专项分析 */
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
					int is_idt  = (ec >> 1) & 1;
					int ext = ec & 1;
					kprintf("Selector index=0x%x  IDT=%d  External=%d\n",
							selector, is_idt, ext);
				}
			}
			break;

		case 8: /* #DF Double Fault */
			kprintf("[Double Fault] System is in an unrecoverable state.\n");
			break;

		case 0: /* #DE Divide Error */
			kprintf("[Divide Error] DIV/IDIV with zero divisor or quotient overflow.\n");
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

	/* 4. 栈回溯(RBP-based，带符号解析) */
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
	uint64_t *rbp = (uint64_t*)frame->rbp;
	for (int depth = 1; depth < 8 && rbp != NULL; depth++) {
		/* 简单有效性检查：地址要在内核虚拟空间范围内 */
		if ((uint64_t)rbp < 0xffff800000000000ULL ||
				(uint64_t)rbp > 0xffffffffffffffffULL ||
				((uint64_t)rbp & 0x7) != 0) {
			kprintf("  #%d rbp=%p (invalid, stopping)\n", depth, rbp);
			break;
		}
		uint64_t ret_addr = rbp[1];
		uint64_t sym_off;
		const char *sym = kallsyms_lookup(ret_addr, &sym_off);
		if (sym) {
			kprintf("  #%d rbp=%p  ret=%p <%s+0x%x>\n",
					depth, rbp, ret_addr, sym, sym_off);
		} else {
			kprintf("  #%d rbp=%p  ret=%p <?+0x0>\n", depth, rbp, ret_addr);
		}
		rbp = (uint64_t*)rbp[0];
	}

	kprintf("================================================\n");
	kprintf("  System Halted.\n");
	kprintf("================================================\n");

	for (;;) {
		__asm__ volatile ("cli; hlt");
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

	idt[num].selector = sel; // 传入 0x08
	idt[num].ist = ist_slot; // 0=不切栈；1-7=使用 TSS.ist[ist_slot-1]
	idt[num].type_attr = flags; // 传入 0x8E
	idt[num].reserved = 0;
}

void idt_ap_init(void)
{
	// CPU 0:执行完整的 idt_init()，填充表项，执行 lidt，初始化 PIC/APIC.
	// CPU 1， 2...:在启动序列中，仅需执行以下语句即可
	extern struct idt_ptr idt_record; // 引用 idt.c 里的记录
	__asm__ volatile ("lidt %0" :: "m"(idt_record));
}

void idt_init(void)
{
	idt_record.limit = (sizeof(struct idt_entry) * 256) - 1;
	idt_record.base = (uintptr_t)&idt;

	// 循环填充前 32 个 CPU 异常
	// 48
	for (int i = 0; i < 48; i++) {
		idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
	}

	// 关键异常走 IST 独立栈，防止栈溢出场景下 triple-fault:
	//   vector  2 (NMI) -> IST_SLOT_NMI
	//   vector  8 (#DF) -> IST_SLOT_DF
	//   vector 18 (#MC) -> IST_SLOT_MCE
	idt_set_gate_ist(2, isr_stub_table[2], 0x08, 0x8E, IST_SLOT_NMI);
	idt_set_gate_ist(8, isr_stub_table[8], 0x08, 0x8E, IST_SLOT_DF);
	idt_set_gate_ist(18, isr_stub_table[18], 0x08, 0x8E, IST_SLOT_MCE);

	__asm__ volatile ("lidt %0" : : "m"(idt_record));

	L_TAG(LOG_BOOT, "IDT loaded.\n");

	// 注册 IPI TLB flush 中断门
	idt_register_ipis();

	// 开启中断，如果已经准备好处理时钟中断等硬件中断
	// 暂不开启，以便处理LAPIC
	// __asm__ volatile ("sti");

	L_TAG(LOG_BOOT, "IDT initialized.\n");
}

// 彻底屏蔽旧的 8259A PIC
// 如果不屏蔽，旧中断可能会干扰 LAPIC
void pic_disable(void)
{
	// 向 PIC 的数据端口写入 0xFF，屏蔽所有 IRQ
	outb(0xA1, 0xFF);
	outb(0x21, 0xFF);
}

/* 捕获键盘中断(IRQ 1)是内核从"静止"到"交互"的关键一步。
 * 但这比处理 CPU 异常多一步：需要和PIC(可编程中断控制器)打交道。
 * 以下是实现键盘中断的详细步骤：
 * 逻辑重构：异常 vs 中断
 *  异常(Exceptions, 0-31):CPU 内部产生
 *  硬件中断(IRQs， 32-47):由外部硬件产生。
 *  默认情况下，硬件中断会和 CPU 异常的中断号冲突
 *  (例如 IRQ 0 默认也是 8)，所以必须先进行PIC重映射(Remap).
 * 第一步：重映射 PIC (8259A)
 *  需要把主片的IRQ映射到 0x20 (32) 开始，
 *  从片映射到 0x28 (40) 开始。
 */
void pic_remap(void)
{
	// 初始化主片和从片
	outb(0x20, 0x11);
	outb(0xA0, 0x11);

	// 设置起始向量号
	outb(0x21, 0x20); // 主片 IRQ 0-7 -> 0x20-0x27
	outb(0xA1, 0x28); // 从片 IRQ 8-15 -> 0x28-0x2F

	// 级联设置
	outb(0x21, 0x04);
	outb(0xA1, 0x02);

	// 设置模式 (8086 模式)
	outb(0x21, 0x01);
	outb(0xA1, 0x01);

	// 屏蔽所有中断 (初始化后先全部关掉，后面再按需开启)
	outb(0x21, 0xFF);
	outb(0xA1, 0xFF);
}

/* 修饰键状态(文件作用域) */
static int kb_shift = 0;
static int kb_ctrl = 0;
static int kb_alt = 0;
static int kb_caps = 0; /* CapsLock 切换状态 */

/* 普通扫描码 -> ASCII(无 Shift) */
static const char scancode_to_ascii[] = {
	0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
	'\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
	0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
	'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

/* Shift 按下时的映射 */
static const char scancode_to_ascii_shifted[] = {
	0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
	'\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
	0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
	'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

void irq_handler(struct interrupt_frame *frame)
{
	uint64_t irq = frame->int_no;

	if (irq == 32) {
		/* 1. 时钟中断 */
		timer_handler(frame);

		cpu_get_ctx()->total_ticks++;
		if (cpu_get_ctx()->current->id == IDLE_PID) {
			cpu_get_ctx()->idle_ticks++;
		}
	} else if (g_e1000_irq_vector && irq == g_e1000_irq_vector) {
		/* 2. e1000 网卡中断 (PCI IRQ → vector 34+) */
		if (g_e1000_irq_handler) {
			g_e1000_irq_handler();
		}
	} else if (irq == 33) {
		/* 3. 键盘中断 */
		uint8_t sc = arch_keyboard_read();
		/* 去掉 break bit,得到 make code */
		uint8_t make = sc & ~SC_BREAK;
		int is_break = (sc & SC_BREAK) != 0;

		/* 修饰键状态维护 */
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

		/* break code 的普通键不产生字符 */
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

		/* 查表 */
		if (make >= sizeof(scancode_to_ascii)) {
			return;
		}

		char c = kb_shift ? scancode_to_ascii_shifted[make] : scancode_to_ascii[make];
		if (c == 0) {
			return;
		}

		/* CapsLock 只影响字母 */
		if (kb_caps) {
			if (c >= 'a' && c <= 'z') {
				c -= 32;
			} else if (c >= 'A' && c <= 'Z') {
				c += 32;
			}
		}

		/* Ctrl 组合键:转换为控制字符 */
		if (kb_ctrl && c >= 'a' && c <= 'z') {
			/* Ctrl+A: 0x01,Ctrl+C: 0x03,以此类推 */
			c -= ('a' - 1);
			/* 在这里可以分发给 tty/shell 层 */
			return;
		}

		if ('c' == c) {
			fb_clear_screen(0);
		}
	} else if (irq == 36) {
		/* 3. COM1 串口输入中断 (IRQ 4 → vector 36) */
		uint8_t lsr = arch_inb(0x3FD);
		if (lsr & 1) {
			serial_input_process(arch_serial_read());
		}
	}
}

/* 说明:
 * - make = sc & ~0x80:break code 就是 make code 的最高位置 1,
 *   这样一张表就能同时处理按下和松开,不需要两张表.
 *   CapsLock 与 Shift 的交互:Shift 按下时 scancode_to_ascii_shifted
 *   已经是大写,CapsLock 再翻转一次,效果是 Shift+CapsLock
 *   打出小写:和真实键盘行为一致.
 * - Ctrl 控制字符:Ctrl+A 到 Ctrl+Z 映射到
 *   0x01~0x1A,这是标准终端约定,后续传给 shell/tty 层可以直接用.
 * - Ctrl+C(0x03),Ctrl+D(0x04),Ctrl+Z(0x1A)等在这里都能正确产生.
 *   kb_alt 暂时只维护状态,Alt 组合键(如 Alt+F4,Alt+方向键)
 *   需要根据用途再扩展.
 */

void debug_print_iretq_frame5(uint64_t rip, uint64_t cs,
		uint64_t rflags, uint64_t user_rsp, uint64_t ss)
{
	kprintf("[iretq] RIP=%p CS=%p RFLAGS=%p USP=%p SS=%p => %s\n",
			rip, cs, rflags, user_rsp, ss,
			(cs & 3) ? "USER" : "!!! KERNEL !!!");
}

// 不用 kprintf,直接写 0xe9 调试端口(每次一个字节)
static void putx(uint64_t val)
{
	for (int i = 60; i >= 0; i -= 4) {
		uint8_t nibble = (val >> i) & 0xF;
		uint8_t c = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
		__asm__ volatile("outb %0, $0xe9" :: "a"(c));
	}
	__asm__ volatile("outb %0, $0xe9" :: "a"((uint8_t)'\n'));
}

void dump_iretq_stack_raw(uint64_t *rsp)
{
	// 直接写端口,不用任何锁/缓冲
	const char *msg = "RAW:";
	for (int i = 0; msg[i]; i++) {
		__asm__ volatile("outb %0, $0xe9" :: "a"((uint8_t)msg[i]));
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
	// IPI
	if (frame->int_no == IPI_VECTOR_TLB) {
		EOI(); // 非常重要:必须先发 EOI

		// 执行 TLB 刷新
		arch_tlb_flush_all();
		L("CPU %d: %s %ld TLB Flushed via IPI", cpu_get_ctx()->id,
				cpu_get_ctx()->current->name, cpu_get_ctx()->current->id);

		/* Notify registered observers (selftest, diagnostics). */
		ipi_tlb_invoke_callbacks();

		return;
	}

	if (frame->int_no == IPI_VECTOR_SELFTEST) {
		EOI();
		ipi_selftest_invoke_callbacks();
		return;
	}

	if (frame->int_no == IPI_VECTOR_RESCHEDULE) {
		/* The target's release-store to need_resched happened before
		 * this IPI.  common_stub checks the flag after we return. */
		EOI();
		ipi_reschedule_ack();
		return;
	}

	if (3 == frame->int_no) {
		panic("int no 3");
		hcf();
	}

	// 1. 处理 CPU 异常 (0-31)
	if (frame->int_no < 32) {
		/* #PF (vec 14): 用户态尝试按需分页恢复；内核态 panic */
		if (frame->int_no == 14) {
			page_fault_handler(frame);
		} else {
			exception_handler(frame);
		}
		return; // 异常处理后通常不发 EOI
	}

	// 2. 处理伪中断 (0xFF)
	if (frame->int_no == 0xFF) {
		// 伪中断不需要发 EOI
		return;
	}

	// 3. 处理所有外部硬件中断 (32-47 以及其他可能的自定义向量)
	// 包括 LAPIC Timer (32), 键盘 (33), 以及未来 IOAPIC 映射的中断
	if (frame->int_no >= 32 && frame->int_no <= 47) {
		// 在 LAPIC 模式下,不论是时钟还是键盘,统一通过 MMIO 发送 EOI.
		// 注意:必须在可能导致上下文切换的 irq_handler 之前发送
		EOI();

		// 处理具体逻辑(如调度,按键处理)
		irq_handler(frame);
		return;
	}

	// 4. 处理系统调用 (0x80)
	if (frame->int_no == 0x80) {
		return;
	}

	// 5. 未知中断
	L("Unknown Interrupt: %ld", frame->int_no);

	EOI();
}

// 声明在汇编中定义的键盘中断入口
extern void irq0();
extern void irq1();
extern void ipi_tlb_entry();
extern void ipi_selftest_entry();
extern void ipi_reschedule_entry();

/**
 * idt_register_ipis() - 注册 IPI 中断门(TLB flush,向量 0xFD)。
 *
 * LAPIC 接管后无需 PIC/PIT 初始化,直接注册 IPI 门即可.
 * 时钟/键盘通过 isr_stub_table 的 irq0/irq1 入口(与 idt_set_gates_default
 * 写入的地址相同),不需要额外 idt_set_gate。
 */
void idt_register_ipis()
{
	// 0xFD 号向量，Ring 0,中断门
	idt_set_gate(IPI_VECTOR_TLB, (uintptr_t)ipi_tlb_entry, 0x08, 0x8E);
	// 0xFC 号，Ring 0，中断门
	idt_set_gate(IPI_VECTOR_SELFTEST, (uintptr_t)ipi_selftest_entry, 0x08, 0x8E);
	// 0xFB 号，Ring 0，远端 runqueue 唤醒
	idt_set_gate(IPI_VECTOR_RESCHEDULE, (uintptr_t)ipi_reschedule_entry,
			0x08, 0x8E);
}
