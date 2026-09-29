/*
 * lock.c - spinlock 调试与自检
 */

#include "lock.h"
#include "cpu.h"
#include "arch_irq.h"
#include "export.h"

/* 初始化 */
void spin_lock_init(spinlock_t *lock)
{
	atomic_set(&lock->locked, 0);
}
EXPORT_SYMBOL(spin_lock_init);

/* 尝试获取锁，非阻塞 */
inline int spin_trylock(spinlock_t *lock)
{
	return (__atomic_exchange_n(&lock->locked.counter, 1, __ATOMIC_ACQ_REL) == 0);
}

inline void spin_lock_raw(spinlock_t *lock)
{
	// 指数退避
	int backoff = 1;

	while (!spin_trylock(lock)) {
		while (__atomic_load_n(&lock->locked.counter, __ATOMIC_RELAXED)) {
			for (int i = 0; i < backoff; i++) {
				cpu_relax();
			}
			if (backoff < 64) {
				backoff <<= 1;
			}
		}
		backoff = 1;
	}
}

inline void spin_lock(spinlock_t *lock)
{
	preempt_disable();

	spin_lock_raw(lock);
}
EXPORT_SYMBOL(spin_lock);

inline void spin_unlock_raw(spinlock_t *lock)
{
	__atomic_store_n(&lock->locked.counter, 0, __ATOMIC_RELEASE);
}

inline void spin_unlock(spinlock_t *lock)
{
	spin_unlock_raw(lock);

	preempt_enable();
}
EXPORT_SYMBOL(spin_unlock);

uint64_t save_and_disable_interrupts(void)
{
	return arch_local_irq_save();
}
EXPORT_SYMBOL(save_and_disable_interrupts);

void restore_interrupts(uint64_t flags)
{
	arch_local_irq_restore(flags);
}
EXPORT_SYMBOL(restore_interrupts);
