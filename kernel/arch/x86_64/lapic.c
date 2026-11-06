/*
 * x86_64 本地 APIC、IOAPIC 与 FPU 初始状态配置。
 *
 * 配置本地 APIC 定时器和 EOI，并通过 IOAPIC 将键盘与 COM1 中断路由到
 * BSP。定时器初始计数尚未根据硬件频率校准。
 */
#include "lapic.h"
#include "vmm.h"
#include "debug.h"
#include "hhdm.h"
#include "log.h"
#include "arch_cpu.h"
#include "serial_arch.h"

/* LAPIC 的映射虚拟地址；寄存器访问当前通过 HHDM 物理映射。 */
uint64_t lapic_base_vaddr;

static inline void lapic_write(uint32_t reg, uint32_t value)
{
	volatile uint32_t *addr = (uint32_t *)phys_to_virt(0xFEE00000 + reg);
	*addr = value;
}

static inline uint32_t lapic_read(uint32_t reg)
{
	volatile uint32_t *addr = (uint32_t *)phys_to_virt(0xFEE00000 + reg);
	return *addr;
}

/*
 * FPU 初始状态模板在每次启动时通过 fninit、ldmxcsr 和 fxsave64 生成，
 * 并由 thread_create_common() 复制到新线程的 fpu_state。全零内容不是有效的
 * FXSAVE 状态：FCW 和 MXCSR 均需采用架构规定的初始值，以屏蔽浮点异常。
 */
uint8_t fpu_initial_state[576] __attribute__((aligned(16)));
static bool fpu_template_ready;

void fpu_init_template(void)
{
	if (fpu_template_ready) {
		return;
	}

	/* 设置 x87 初始状态：FCW=0x037F、FSW=0，所有 ST 寄存器为空。 */
	__asm__ volatile("fninit");
	/* 设置 MXCSR=0x1F80：屏蔽异常并采用 round-to-nearest 舍入模式。 */
	__asm__ volatile("ldmxcsr %0" ::"m"(*(const uint32_t[]){0x1F80}));
	/* 清零 XMM0-XMM15。fninit 只重置 x87 状态；若不清零，fxsave64 会保存
	 * 固件遗留值，并将其带入每个新线程的初始状态。 */
	__asm__ volatile("pxor %%xmm0,  %%xmm0\n	"
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
			 "pxor %%xmm15, %%xmm15" ::
				 : "memory");
	/* 保存完整的 FPU 初始状态模板。 */
	__asm__ volatile("fxsave64 %0" : "=m"(fpu_initial_state)::"memory");

	fpu_template_ready = true;
}

/*
 * 启用 x87/SSE 状态保存指令，并建立新线程使用的初始 FXSAVE 状态模板。
 *
 * Limine bootloader 初始化的 CR4=0x20（仅 PAE），缺少 OSFXSR(bit 9)。
 * fxsave64/fxrstor64 指令要求 CR4.OSFXSR=1，否则触发 #GP(0)。
 *
 * 同时设置 OSXMMEXCPT(bit 10)，允许 SIMD 浮点异常通过 #XF 投递。
 *
 * CR0 清理：
 *   - EM(bit 2)=0  禁止 x87 opcode 触发 #NM，直接原生执行
 *   - TS(bit 3)=0  禁止 x87/MMX/SSE 首条指令触发 #NM（eager switching
 *                   下 FPU 状态已在 switch_to 中完成 save/restore）
 *   - MP(bit 1)=1  标准 x87 存在标志
 *   - NE(bit 5)=1  x87 错误走原生 #MF 异常，不出 IRQ13（本内核无 PIC）
 *
 * 每个 CPU 必须在使用 FXSAVE/FXRSTOR 前调用。首次调用还生成共享初始状态模板。
 */
void cpu_enable_fpu(void)
{
	uint64_t cr0 = arch_read_cr0();
	cr0 |= (1ULL << 1);  /* CR0.MP: math present */
	cr0 &= ~(1ULL << 2); /* CR0.EM: clear — native x87, don't emulate */
	cr0 &= ~(1ULL << 3); /* CR0.TS: clear — no #NM on first FPU use */
	cr0 |= (1ULL << 5);  /* CR0.NE: native x87 error handling */
	__asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");

	uint64_t cr4 = arch_read_cr4();
	cr4 |= (1ULL << 9);  // CR4.OSFXSR
	cr4 |= (1ULL << 10); // CR4.OSXMMEXCPT
	__asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");

	/* 仅首次调用时生成 FXSAVE 模板；后续调用由 fpu_template_ready 检查跳过。 */
	fpu_init_template();
}

void lapic_init()
{
	/* 启用 FPU 状态保存指令；缺少 CR4.OSFXSR 时 FXSAVE/FXRSTOR 会触发 #UD。 */
	cpu_enable_fpu();

	/* 软件启用 LAPIC，并将伪中断向量设为 0xFF。 */
	lapic_write(LAPIC_REG_SPURIOUS, lapic_read(LAPIC_REG_SPURIOUS) | 0x1FF);

	/* Divide Configuration 编码 0xB 表示除数为 1。 */
	lapic_write(LAPIC_REG_TIMER_DIV, 0xB);

	/* 使用向量 32，并启用周期模式。 */
	lapic_write(LAPIC_REG_LVT_TIMER, 32 | (1 << 17));

	/* 设置初始计数；该值尚未校准为固定时钟频率。 */
	lapic_write(LAPIC_REG_TIMER_INIT, 10000000);

	static uint32_t k = 0;
	if (++k == g_cpu_count) {
		L_TAG(LOG_BOOT, "lapic initialized for all CPUs.\n");
	}
}

/* 将 LAPIC 和 IOAPIC 的 MMIO 区域映射到页表，并设置为不可缓存访问。 */
void lapic_map(uint64_t *pml4)
{
	// 保存虚拟基地址，供后续寄存器访问使用。
	lapic_base_vaddr = (uint64_t)phys_to_virt(LAPIC_BASE_PHYS);

	// 映射 LAPIC 寄存器区域并禁用缓存。
	vmm_map(pml4, lapic_base_vaddr, LAPIC_BASE_PHYS, PTE_PRESENT | PTE_WRITABLE | (1ULL << 4));

	// 以相同属性映射 IOAPIC 寄存器区域。
	uint64_t ioapic_vaddr = (uint64_t)phys_to_virt(IOAPIC_BASE_PHYS);
	vmm_map(pml4, ioapic_vaddr, IOAPIC_BASE_PHYS, PTE_PRESENT | PTE_WRITABLE | (1ULL << 4));

	L("LAPIC mapped at virtual %p (PCD enabled)", (void *)lapic_base_vaddr);
}

/* 向 LAPIC 确认当前本地中断已处理。 */
void lapic_eoi()
{
	// 调用方须在启动中断前初始化 HHDM，并确保 LAPIC 地址已映射。
	lapic_write(LAPIC_REG_EOI, 0);
}

/* 通过 IOREGSEL/IOWIN 间接访问 IOAPIC 寄存器。 */
inline uint32_t ioapic_read(uint8_t reg)
{
	volatile uint32_t *ioapic = (uint32_t *)phys_to_virt(IOAPIC_BASE_PHYS);
	ioapic[IOREGSEL / 4] = reg;
	return ioapic[IOWIN / 4];
}

inline void ioapic_write(uint8_t reg, uint32_t data)
{
	volatile uint32_t *ioapic = (uint32_t *)phys_to_virt(IOAPIC_BASE_PHYS);
	ioapic[IOREGSEL / 4] = reg;
	ioapic[IOWIN / 4] = data;
}

/* 设置 IOAPIC 重定向表项。
 *
 * irq      是 IOAPIC 输入线路号，vector 是目标 IDT 向量，lapic_id 是
 * 目标处理器的硬件 APIC ID。当前启动路径将键盘和串口路由到 ID 0。
 */
void ioapic_set_entry(uint8_t irq, uint8_t vector, uint32_t lapic_id)
{
	uint8_t reg = 0x10 + irq * 2;

	uint64_t entry = (uint64_t)vector | // 中断向量。
			 (0ULL << 8) |	    // 固定投递模式。
			 (0ULL << 11) |	    // 物理目标模式。
			 (0ULL << 13) |	    // 投递状态位由硬件维护。
			 (0ULL << 15) |	    // 边沿触发。
			 (0ULL << 16) |	    // Remote IRR 初始为零。
			 (0ULL << 17) |	    // 高电平有效。
			 (0ULL << 48);	    // 解除屏蔽。

	// 低 32 位包含向量号及投递、触发和屏蔽等控制位。
	ioapic_write(reg, (uint32_t)entry);
	// 高 32 位的 bit 24–31 保存目标 APIC ID。
	ioapic_write(reg + 1, lapic_id << 24);
}

void keyboard_init_hw()
{
	/* 清空控制器输出缓冲区中的遗留数据。 */
	while (inb(0x64) & 1) {
		inb(0x60);
	}

	/* 等待控制器输入缓冲区可接收命令。 */
	while (inb(0x64) & 2) {
	}

	/* 启用第一组 PS/2 端口。 */
	outb(0x64, 0xAE);

	/* 等待控制器处理命令。 */
	while (inb(0x64) & 2) {
	}

	/* 读取 8042 配置字节并启用键盘 IRQ。 */
	outb(0x64, 0x20); // 请求读取 8042 配置字节。
	uint8_t config = inb(0x60);
	/* 配置字节 bit 0 控制键盘 IRQ。 */
	config |= 0x01;
	/* 保留其他控制位；bit 7 为保留位。 */
	config &= ~0x80;

	/* 写回 8042 配置字节。 */
	while (inb(0x64) & 2) {
	}
	outb(0x64, 0x60); // 请求写入 8042 配置字节。
	outb(0x60, config);

	/* 请求键盘开始扫描。 */
	while (inb(0x64) & 2) {
	}
	outb(0x60, 0xF4);

	/* 等待键盘确认响应。 */
	while ((inb(0x64) & 1) == 0) {
	}
	if (inb(0x60) != 0xFA) {
		panic("Keyboard ACK failed!");
	}

	L_TAG(LOG_BOOT, "keyboard controller fully initialized (IRQ1 enabled).\n");
}

void ioapic_init()
{
	keyboard_init_hw();
	/* 将键盘 IRQ1 路由到向量 33，并投递给 APIC ID 0。 */
	ioapic_set_entry(1, 33, 0);

	/* 将 COM1 IRQ4 路由到向量 36，并投递给 APIC ID 0。 */
	arch_serial_init_com1();
	ioapic_set_entry(4, 36, 0);
}

uint32_t get_cpu_id()
{
	/* LAPIC 映射尚未就绪时，从 CPUID 读取初始 APIC ID。 */
	if (lapic_base_vaddr == 0) {
		return get_cpu_id_cpuid();
	}

	/* 读取 LAPIC ID 寄存器（偏移 0x20）。 */
	volatile uint32_t *lapic_id_reg = (uint32_t *)(lapic_base_vaddr + 0x20);
	uint32_t raw = *lapic_id_reg;
	/* xAPIC 模式下 APIC ID 位于寄存器高 8 位。 */
	uint32_t apic_id = raw >> 24;

	return apic_id;
}

/* 启动早期或 LAPIC 尚不可访问时，通过 CPUID 读取初始 APIC ID。 */
uint32_t get_cpu_id_cpuid()
{
	return arch_cpu_apic_id();
}
