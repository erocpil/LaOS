/*
 * arch_irq.h - x86_64 中断/CPU 状态原语
 *
 * 命名沿用 Linux arch/x86/include/asm/irqflags.h，让熟悉 Linux 的读者
 * 一眼能对上。本文件只暴露被 kernel 真实调用的接口；arch 内部代码
 * (main.c / syscall.c / cpu.c 等)直接写 inline asm，不绕这一层。
 *
 * 本文件提供的本地 IRQ 与 CPU 等待原语：
 *   arch_local_save_flags     读取 RFLAGS，不改中断状态
 *   arch_local_irq_save       cli + pushfq(保存 RFLAGS.IF)
 *   arch_local_irq_restore    popfq(恢复 RFLAGS.IF)
 *   arch_local_irq_disable    cli
 *   arch_local_irq_enable     sti
 *   arch_cpu_halt             hlt(等中断)
 */

#ifndef __ARCH_IRQ_H__
#define __ARCH_IRQ_H__

#include <stdint.h>

#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif

static __always_inline unsigned long arch_local_save_flags(void)
{
	unsigned long flags;
	__asm__ volatile("pushfq; pop %0" : "=rm"(flags) : : "memory");
	return flags;
}

static __always_inline void arch_local_irq_restore(unsigned long flags)
{
	__asm__ volatile("push %0; popfq" : : "rm"(flags) : "memory", "cc");
}

static __always_inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags;
	/* 在一个 asm 块中先保存 RFLAGS，再关闭 IRQ；CLI 前仍可能响应 IRQ。 */
	__asm__ volatile("pushfq; cli; pop %0" : "=rm"(flags) : : "memory");
	return flags;
}

static __always_inline void arch_local_irq_disable(void)
{
	__asm__ volatile("cli" ::: "memory");
}

static __always_inline void arch_local_irq_enable(void)
{
	__asm__ volatile("sti" ::: "memory");
}

/* x86_64 PCI IRQ 路由（IOAPIC）。 */

#define PCI_IRQ_VECTOR_BASE 34
#define PCI_IRQ_VECTOR_MAX  47
#define PCI_IRQ_COUNT	    (PCI_IRQ_VECTOR_MAX - PCI_IRQ_VECTOR_BASE + 1)

void ioapic_set_entry(uint8_t irq, uint8_t vector, uint32_t lapic_id);

/* 将 PCI IRQ 线路经 IOAPIC 路由到 IDT；由通用 pci.c 的 pci_enable_intx() 调用。 */
static __always_inline int arch_pci_irq_enable(uint32_t irq_id)
{
	if (irq_id >= PCI_IRQ_COUNT) {
		return -1;
	}

	uint8_t vector = (uint8_t)(PCI_IRQ_VECTOR_BASE + irq_id);
	ioapic_set_entry((uint8_t)irq_id, vector, 0);

	return 0;
}

static __always_inline void arch_cpu_halt(void)
{
	__asm__ volatile("hlt" ::: "memory");
}

/* 开启本地 IRQ 并立即执行 HLT。STI 延迟到下一条指令后才允许响应
 * 可屏蔽中断，因此该序列可用于避免 idle 检查与休眠之间的丢唤醒窗口。 */
static __always_inline void arch_cpu_safe_halt(void)
{
	__asm__ volatile("sti; hlt" ::: "memory");
}

#endif /* __ARCH_IRQ_H__ */
