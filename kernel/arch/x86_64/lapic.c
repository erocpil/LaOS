/*
 * lapic.c - 本地 APIC 与 IOAPIC 驱动
 *
 * LAPIC:初始化，定时器(100Hz)，EOI.
 * IOAPIC:重定向表项配置(当前仅键盘引脚->vector 33).
 */
#include "lapic.h"
#include "vmm.h"
#include "printf.h"
#include "define.h"
#include "debug.h"
#include "hhdm.h"
#include "log.h"
#include "arch_cpu.h"
#include "serial_arch.h"

// 通过 HHDM 偏移映射到虚拟地址
uint64_t lapic_base_vaddr;

static inline void lapic_write(uint32_t reg, uint32_t value)
{
	volatile uint32_t* addr = (uint32_t*)phys_to_virt(0xFEE00000 + reg);
	*addr = value;
}

static inline uint32_t lapic_read(uint32_t reg)
{
	volatile uint32_t* addr = (uint32_t*)phys_to_virt(0xFEE00000 + reg);
	return *addr;
}

/**
 * FPU initial state template — generated once per boot via
 * fninit + ldmxcsr + fxsave64.  Copied to each new thread's
 * fpu_state in thread_create_common().  All-zeros is NOT a
 * valid FXSAVE image: FCW would be 0x0000 instead of 0x037F,
 * MXCSR would be 0x00000000 instead of 0x1F80, leaving x87/SSE
 * exceptions unmasked.
 */
uint8_t fpu_initial_state[576] __attribute__((aligned(16)));
static bool fpu_template_ready;

void fpu_init_template(void)
{
	if (fpu_template_ready)
		return;

	/* fninit → FCW=0x037F, FSW=0x0000, all ST registers empty */
	__asm__ volatile("fninit");
	/* ldmxcsr → MXCSR=0x1F80 (all exceptions masked, round-nearest) */
	__asm__ volatile("ldmxcsr %0" :: "m"(*(const uint32_t[]){0x1F80}));
	/* Zero XMM0-XMM15.  fninit only resets x87 state; XMM registers
	 * retain boot-firmware residue.  Without explicit zeroing, fxsave64
	 * would capture undefined values and leak them into every new thread. */
	__asm__ volatile(
		"pxor %%xmm0,  %%xmm0\n	"
		"pxor %%xmm1,  %%xmm1\n	"
		"pxor %%xmm2,  %%xmm2\n	"
		"pxor %%xmm3,  %%xmm3\n	"
		"pxor %%xmm4,  %%xmm4\n	"
		"pxor %%xmm5,  %%xmm5\n	"
		"pxor %%xmm6,  %%xmm6\n	"
		"pxor %%xmm7,  %%xmm7\n	"
		"pxor %%xmm8,  %%xmm8\n	"
		"pxor %%xmm9,  %%xmm9\n	"
		"pxor %%xmm10, %%xmm10\n	"
		"pxor %%xmm11, %%xmm11\n	"
		"pxor %%xmm12, %%xmm12\n	"
		"pxor %%xmm13, %%xmm13\n	"
		"pxor %%xmm14, %%xmm14\n	"
		"pxor %%xmm15, %%xmm15"
		::: "memory"
	);
	/* fxsave64 captures the complete init state */
	__asm__ volatile("fxsave64 %0" : "=m"(fpu_initial_state) :: "memory");

	fpu_template_ready = true;
}

/**
 * cpu_enable_fpu - 设置 CR4.OSFXSR 和 CR4.OSXMMEXCPT，启用 fxsave/fxrstor。
 *
 * Limine bootloader 初始化的 CR4=0x20（仅 PAE），缺少 OSFXSR(bit 9)。
 * fxsave64/fxrstor64 指令要求 CR4.OSFXSR=1，否则触发 #GP(0)。
 *
 * 同时设 OSXMMEXCPT(bit 10) 以支持 #XF 异常投递；当前 -mno-sse
 * 下编译器不生成 SSE 指令，但 e1000 等模块可能使用。
 *
 * CR0 清理：
 *   - EM(bit 2)=0  禁止 x87 opcode 触发 #NM，直接原生执行
 *   - TS(bit 3)=0  禁止 x87/MMX/SSE 首条指令触发 #NM（eager switching
 *                   下 FPU 状态已在 switch_to 中完成 save/restore）
 *   - MP(bit 1)=1  标准 x87 存在标志
 *   - NE(bit 5)=1  x87 错误走原生 #MF 异常，不出 IRQ13（本内核无 PIC）
 *
 * 此函数在 lapic_init() 开头被调用，确保每 CPU 在第一次上下文切换前
 * 已开启 FPU 支持。BSP 调用还会生成 fpu_initial_state 模板。
 */
void cpu_enable_fpu(void)
{
	uint64_t cr0 = arch_read_cr0();
	cr0 |= (1ULL << 1);   /* CR0.MP: math present */
	cr0 &= ~(1ULL << 2);  /* CR0.EM: clear — native x87, don't emulate */
	cr0 &= ~(1ULL << 3);  /* CR0.TS: clear — no #NM on first FPU use */
	cr0 |= (1ULL << 5);   /* CR0.NE: native x87 error handling */
	__asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

	uint64_t cr4 = arch_read_cr4();
	cr4 |= (1ULL << 9);   // CR4.OSFXSR
	cr4 |= (1ULL << 10);  // CR4.OSXMMEXCPT
	__asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

	/* Generate the standard FXSAVE template once (BSP only —
	 * fpu_template_ready guard makes subsequent calls a no-op). */
	fpu_init_template();
}

void lapic_init()
{
	// 0. 开启 FPU 支持：不设则 fxsave64/fxrstor64 触发 #GP(0)
	cpu_enable_fpu();

	// 1. 软件开启 LAPIC，并设置伪中断向量(通常设为 0xFF)
	// 0x100 是使能位 (Software Enable)
	lapic_write(LAPIC_REG_SPURIOUS, lapic_read(LAPIC_REG_SPURIOUS) | 0x1FF);

	// 2. 配置定时器分频系数 (Divide Configuration Register)
	// 0x3 表示分频值为 16
	/*
	 * 逻辑风险:LAPIC 的 Divide Configuration Register 的位分布比较特殊。虽然 0x3 在很多手册中代表分频 16，但有些处理器要求该寄存器的位 2 必须为 0 才是 16 分频。

	 建议：通常建议使用 0xB(分频 1)作为初始测试，或者严格按照手册将位 3 清零。
	 */
	lapic_write(LAPIC_REG_TIMER_DIV, 0xB);

	// 3. 配置 LVT Timer 寄存器
	// 向量号设为 32 (IRQ0)，模式设为 Periodic (位 17 置 1)
	lapic_write(LAPIC_REG_LVT_TIMER, 32 | (1 << 17));

	// 4. 设置初始计数初始值 (Init Count)
	// 这个值决定了频率。在没有校准前，设为一个较大的值。
	lapic_write(LAPIC_REG_TIMER_INIT, 10000000);

	static uint32_t k = 0;
	if (++k == g_cpu_count) {
		L_TAG(LOG_BOOT, "lapic initialized for all CPUs.\n");
	}
}

/* 0xFEE00000 是一个固定的物理地址(Physical Address)，不是虚拟地址。
 * 它是 x86/x86-64 处理器中 Local APIC(本地高级可编程中断控制器)
 * 寄存器的默认内存映射地址(Memory-Mapped I/O，简称 MMIO).
 */
void lapic_map(uint64_t *pml4)
{
	// 保存虚拟基地址，供后面读写使用
	lapic_base_vaddr = (uint64_t)phys_to_virt(LAPIC_BASE_PHYS);

	// 映射 LAPIC(必须加 PCD 禁用缓存)
	// PCD = 1
	vmm_map(pml4, lapic_base_vaddr, LAPIC_BASE_PHYS,
			PTE_PRESENT | PTE_WRITABLE | (1ULL << 4));

	// 映射 IOAPIC(同理)
	uint64_t ioapic_vaddr = (uint64_t)phys_to_virt(IOAPIC_BASE_PHYS);
	vmm_map(pml4, ioapic_vaddr, IOAPIC_BASE_PHYS,
			PTE_PRESENT | PTE_WRITABLE | (1ULL << 4));

	L("LAPIC mapped at virtual %p (PCD enabled)", (void*)lapic_base_vaddr);
}

// 每次时钟中断结束时必须调用，否则中断不再触发
void lapic_eoi()
{
	// 必须确保 hhdm_offset() 在中断发生时已经被正确赋值
	// 且映射了 0xFEE00000 区域
	lapic_write(LAPIC_REG_EOI, 0);
}

// 获取 IOAPIC 的虚拟地址
inline uint32_t ioapic_read(uint8_t reg)
{
	volatile uint32_t *ioapic = (uint32_t*)phys_to_virt(IOAPIC_BASE_PHYS);
	ioapic[IOREGSEL / 4] = reg;
	return ioapic[IOWIN / 4];
}

inline void ioapic_write(uint8_t reg, uint32_t data)
{
	volatile uint32_t *ioapic = (uint32_t*)phys_to_virt(IOAPIC_BASE_PHYS);
	ioapic[IOREGSEL / 4] = reg;
	ioapic[IOWIN / 4] = data;
}

/** ioapic_set_entry - 设置 IOAPIC 重定向表项
 *
 * irq     : 物理引脚号(键盘通常是 1)
 * vector  : 对应的 IDT 向量号(我们定为 33)
 * lapic_id: 目标 CPU 的 LAPIC ID(0 = BSP)
 */
void ioapic_set_entry(uint8_t irq, uint8_t vector, uint32_t lapic_id)
{
	uint8_t reg = 0x10 + irq * 2;

	uint64_t entry = (uint64_t)vector |          // vector
		(0ULL << 8) |               // Delivery Mode = Fixed (000)
		(0ULL << 11) |              // Destination Mode = Physical (0)
		(0ULL << 13) |              // Delivery Status
		(0ULL << 15) |              // Trigger Mode = Edge
		(0ULL << 16) |              // Remote IRR
		(0ULL << 17) |              // Interrupt Input Pin Polarity = Active High
		(0ULL << 48);               // Mask = 0 (unmasked)

	// 低 32 位：向量号和模式位
	ioapic_write(reg, (uint32_t)entry);
	// 高 32 位 bits 24-31：Destination Field = APIC ID
	ioapic_write(reg + 1, lapic_id << 24);
}

void keyboard_init_hw()
{
	// 1. 清空输入缓冲区(防止旧数据干扰)
	while (inb(0x64) & 1) {
		inb(0x60);
	}

	// 2. 等待命令缓冲区空
	while (inb(0x64) & 2) { /* spin */ }

	// 3. 开启键盘端口
	outb(0x64, 0xAE);

	// 4. 等待命令被接受
	while (inb(0x64) & 2) {}

	// 5. [关键]读取并设置 8042 配置字节，打开键盘中断使能位
	outb(0x64, 0x20);                // Read Command Byte
	uint8_t config = inb(0x60);
	// bit 0 = Keyboard Interrupt Enable
	config |= 0x01;
	// config |= 0x02; // 如果以后要鼠标，可以再加这一行
	config &= ~0x80; // bit 7 通常清零(不翻译 IRQ)

	// 写回配置字节
	while (inb(0x64) & 2) {}
	outb(0x64, 0x60); // Write Command Byte
	outb(0x60, config);

	// 6. (推荐)给键盘设备发送 0xF4 启用扫描(大多数键盘都需要)
	while (inb(0x64) & 2) {}
	outb(0x60, 0xF4);

	// 等待键盘 ACK(可选，但更稳健)
	while ((inb(0x64) & 1) == 0) {}
	if (inb(0x60) != 0xFA) {
		panic("Keyboard ACK failed!");
	}

	L_TAG(LOG_BOOT, "keyboard controller fully initialized (IRQ1 enabled).\n");
}

void ioapic_init()
{
	keyboard_init_hw();
	// 1. 确保已经在 VMM 中映射了 0xFEC00000 区域
	// vmm_map(kernel_pml4, hhdm_offset() + 0xFEC00000, 0xFEC00000, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);

	// 2. 映射键盘：物理引脚 IRQ 1 -> IDT 向量 33
	// 这里我们将它投递给 CPU 0 (主核)
	ioapic_set_entry(1, 33, 0);

	// COM1 串口 (物理引脚 IRQ 4 -> IDT 向量 36)
	arch_serial_init_com1();
	ioapic_set_entry(4, 36, 0);
}

uint32_t get_cpu_id()
{
	// 如果还没映射 HHDM，直接 fallback 到 CPUID
	if (lapic_base_vaddr == 0) {
		// kprintf("[get_cpu_id] LAPIC not mapped yet, fallback to CPUID\n");
		return get_cpu_id_cpuid();
	}

	// 对于 Limine，建议直接使用其 MP 响应中提供的 ID，而不是每次去读 MMIO .
	// 访问 LAPIC 的 ID 寄存器 (偏移量 0x20)
	volatile uint32_t* lapic_id_reg = (uint32_t*)(lapic_base_vaddr + 0x20);
	uint32_t raw = *lapic_id_reg;
	// 高 8 位(或更多，取决于架构)是实际 ID
	uint32_t apic_id = raw >> 24;

	return apic_id;
}

/* TODO */
/* 方法 B:利用 gs 寄存器(高性能内核常用)
   像 Linux 或 Windows 内核，会为每个 CPU 分配一块内存(Per-CPU Area)，然后把这块内存的地址存在 gs(或 fs)寄存器中。

   在初始化每个核时，用 wrmsr 指令将 CPU 结构体的地址写入 IA32_GS_BASE.

   之后获取 ID 只需一行汇编:mov %gs:offset， %rax.这比读取 MMIO 快得多。
   */
// 用 CPUID(最稳，推荐在启动早期使用)
uint32_t get_cpu_id_cpuid()
{
	return arch_cpu_apic_id();
}
