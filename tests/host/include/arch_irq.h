#pragma once
/* Host parser tests must never execute privileged IRQ operations. */
static inline unsigned long arch_local_irq_save(void)
{
	__builtin_trap();
	return 0;
}
static inline void arch_local_irq_restore(unsigned long flags)
{
	(void)flags;
	__builtin_trap();
}
