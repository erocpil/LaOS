#ifndef __LOCK_IRQ_H__
#define __LOCK_IRQ_H__

/*
 * lock.h - spinlock 类型与操作宏
 */

#include "atomic.h"
#include "arch_irq.h"
#include "arch_barrier.h"

typedef struct {
	atomic_t locked;
} spinlock_t;

#define SPINLOCK_INIT()  { ATOMIC_INIT(0) }

/*
 * Spinlock API
 *
 * LaOS 提供三套 spinlock 操作，语义逐层加重：
 *
 * spin_lock_raw / spin_unlock_raw
 *   裸锁。只做 CAS/xchg 自旋，不关中断，不关抢占。
 *   适用场景：调度器内部(pick_next 已关中断)，中断下半部，
 *   或调用方已通过其他机制保证不会被当前 CPU 抢占/中断。
 *
 * spin_lock / spin_unlock
 *   自动关抢占。内部调 spin_lock_raw + preempt_disable/enable.
 *   适用场景：非中断上下文，非调度器临界区，持锁期间不希望被抢占。
 *   注意：不关中断。若 ISR 可能竞争同一锁，需改用 _irqsave 版本。
 *
 * arch_spin_lock_irqsave / arch_spin_unlock_irqrestore
 *   关中断 + 拿锁。保存架构中断状态后再自旋获取锁；释放后精确还原。
 *   适用场景：锁可能在中断上下文竞争(mutex，pmm，vmm 等)，
 *   或调用方需要同时保证不被中断抢占不被其他 CPU 并发。
 */

void spin_lock_init(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
void spin_lock_raw(spinlock_t *lock);
void spin_unlock_raw(spinlock_t *lock);
int spin_trylock(spinlock_t *lock);
uint64_t save_and_disable_interrupts(void);
void restore_interrupts(uint64_t irq_flags);

/**
 * arch_spin_lock_irqsave() / arch_spin_unlock_irqrestore()
 *
 * 设计原则
 * 1. 接口对称：两处均用 uint64_t flags(值)，
 *    取地址操作由宏统一处理，调用方写法一致。
 * 2. 先关闭本地中断再拿锁，保证持锁期间不被本 CPU IRQ 打断。
 * 3. 先 unlock 再恢复中断状态：锁释放对其他 CPU 可见后，
 *    再恢复本 CPU 中断状态，是内核标准顺序。
 * 4. 精确还原原状态，而非无条件开中断，避免调用者
 *    原本就处于关中断状态时被意外打开。
 */
static inline void __arch_spin_lock_irqsave(spinlock_t *lock, uint64_t *flags)
{
	// 保存并关闭本地中断
	*flags = arch_local_irq_save();

	// 关中断后获取锁；本 CPU 中断已关，不会被自己的中断处理打断
	while (__sync_lock_test_and_set(&lock->locked.counter, 1)) {
		cpu_relax();
	}
}

static inline void __arch_spin_unlock_irqrestore(spinlock_t *lock, uint64_t irq_flags)
{
	// 1. 释放锁
	__sync_lock_release(&lock->locked.counter);

	// 2. 精确还原调用者原有的中断状态
	arch_local_irq_restore(irq_flags);
}

#define arch_spin_lock_irqsave(lock, flags)      \
	__arch_spin_lock_irqsave((lock), &(flags))

#define arch_spin_unlock_irqrestore(lock, flags)      \
	__arch_spin_unlock_irqrestore((lock), (flags))

#endif
