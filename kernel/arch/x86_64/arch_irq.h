/*
 * arch_irq.h - x86_64 中断/CPU 状态原语
 *
 * 命名沿用 Linux arch/x86/include/asm/irqflags.h，让熟悉 Linux 的读者
 * 一眼能对上。本文件只暴露被 kernel 真实调用的接口；arch 内部代码
 * (main.c / syscall.c / cpu.c 等)直接写 inline asm，不绕这一层。
 *
 * 五个原语：
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
	__asm__ volatile("push %0; popfq"
			 :
			 : "rm"(flags)
			 : "memory", "cc");
}

static __always_inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags;
	/* pushfq + cli + pop 之间不可被中断打断，三条指令同一 asm 块 */
	__asm__ volatile("pushfq; cli; pop %0"
			 : "=rm"(flags)
			 :
			 : "memory");
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

/* ---- x86_64 PCI IRQ routing (IOAPIC) ---- */

#define PCI_IRQ_VECTOR_BASE 34
#define PCI_IRQ_VECTOR_MAX  47
#define PCI_IRQ_COUNT       (PCI_IRQ_VECTOR_MAX - PCI_IRQ_VECTOR_BASE + 1)

void ioapic_set_entry(uint8_t irq, uint8_t vector, uint32_t lapic_id);

/* Route a PCI IRQ line through IOAPIC to the IDT.
 * Called from pci_enable_intx() in shared pci.c. */
static __always_inline int arch_pci_irq_enable(uint32_t irq_id)
{
	if (irq_id >= PCI_IRQ_COUNT)
		return -1;

	uint8_t vector = (uint8_t)(PCI_IRQ_VECTOR_BASE + irq_id);
	ioapic_set_entry((uint8_t)irq_id, vector, 0);
	return 0;
}

static __always_inline void arch_cpu_halt(void)
{
	__asm__ volatile("hlt" ::: "memory");
}

/*
 * arch_cpu_safe_halt -- 原子化的 sti + hlt 序列，消除 idle 路径丢中断窗口。
 *
 * x86 保证 sti 指令执行后的下一条指令在执行前不会被中断打断。
 * 因此 "sti； hlt" 是一个不可分割的操作：开中断后立即进入低功耗等待，
 * 中断一定会在 hlt 之后的下一条指令才被响应，从而唤醒 CPU.
 *
 * 如果不用 sti；hlt 而用分开的 sti -> hlt(即当前 arch_cpu_halt):
 *
 *   关中断状态检查 runqueue:
 *     if (atomic64_read(&runqueue.count) > 0)
 *         schedule();
 *     // <--- 中断在此刻到达，handler 把新线程入队并置 need_resched
 *     sti;             // 开中断(但中断已在 sti 前投递，IF 刚置位就响应)
 *     hlt;             // CPU 已 halt，等待下一次中断----但下一次中断可能永远不来
 *
 * sti；hlt 原子序列消除了这个窗口:sti 执行后 hlt 立即执行，
 * 中断在 hlt 后响应，CPU 被唤醒，不会丢事件。
 *
 * 此接口替代 idle 循环中当前的 arch_cpu_halt().
 */
static __always_inline void arch_cpu_safe_halt(void)
{
	__asm__ volatile("sti; hlt" ::: "memory");
}

#endif /* __ARCH_IRQ_H__ */
