#ifndef __ARCH_CPU_H__
#define __ARCH_CPU_H__

#include <stdint.h>

#include "export.h"

/* -------------------------------------------------------------
 * arch_cpu.h - x86_64 CPU 指令包装
 *
 * 范围:CR2 / TSC / CPUID 等读取本核状态的指令。
 * 不含:CR3 (-> arch_tlb.h) / IRQ (-> arch_irq.h) / barrier (-> arch_barrier.h).
 *
 * 命名约定：
 *   - 通用 Linux 风格名 (rdtsc) 沿用，便于跨项目阅读
 *   - LaOS 自有的，调用方语义专一的包装走 arch_ 前缀
 *     (arch_read_cr2 / arch_cpu_apic_id)
 * ------------------------------------------------------------- */

#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif

/* rdtsc - 读取 64 位 Time Stamp Counter
 * EDX:EAX = TSC，需在 union 中拼装 */
static inline uint64_t rdtsc(void)
{
	union {
		uint64_t tsc_64;
		struct {
			uint32_t lo_32;
			uint32_t hi_32;
		};
	} tsc;

	__asm__ volatile("rdtsc" : "=a"(tsc.lo_32), "=d"(tsc.hi_32));

	return tsc.tsc_64;
}
EXPORT_SYMBOL(rdtsc);

/* arch_read_cr2 - 读取 CR2 (页错误线性地址)
 * #PF 进入后必须最先读取，防止后续代码触发新缺页覆盖此寄存器 */
static __always_inline uint64_t arch_read_cr2(void)
{
	uint64_t val;

	__asm__ volatile("mov %%cr2, %0" : "=r"(val));

	return val;
}

static __always_inline uint64_t arch_read_cr0(void)
{
	uint64_t val = 0;

	__asm__ volatile ("mov %%cr0, %0" : "=r"(val));

	return val;
}

static __always_inline uint64_t arch_read_cr4(void)
{
	uint64_t val = 0;

	__asm__ volatile ("mov %%cr4, %0" : "=r"(val));

	return val;
}

/* arch_cpu_apic_id - 通过 CPUID leaf 1 读取本核初始 APIC ID
 * CPUID(EAX=1) -> EBX[31:24] = Initial APIC ID (xAPIC 模式下足够)
 * 早期启动 / LAPIC MMIO 未就绪时使用 */
static __always_inline uint32_t arch_cpu_apic_id(void)
{
	uint32_t ebx;

	__asm__ volatile("cpuid" : "=b"(ebx) : "a"(1) : "ecx", "edx");

	return ebx >> 24;
}


static inline void arch_smp_probe_report(void) {}

/* Called early in PMM init: per-CPU GS base for x86_64 */
void arch_cpu_early_init(void);

/* x86_64 MSI: APIC-based FSB -- not implemented yet */
static inline int arch_pci_msi_enable(uint8_t bus, uint8_t slot, uint8_t func,
		uint64_t *msi_addr, uint32_t *msi_data)
{
	(void)bus; (void)slot; (void)func;
	(void)msi_addr; (void)msi_data;
	return -1;
}

/* inb/outb -- x86 IO port access primitives */
static inline uint8_t inb(uint16_t port)
{
	uint8_t ret;
	__asm__ volatile ("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
	return ret;
}

static inline void outb(uint16_t port, uint8_t val)
{
	__asm__ volatile ("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

#endif
