#ifndef __MUTEX_H__
#define __MUTEX_H__

/*
 * mutex.h - mutex/rwlock 定义
 */

#include <stdint.h>

#include "list.h"
#include "lock.h"

/*
 * CONFIG_MUTEX_HANDOFF: mutex 锁移交策略编译期开关。
 *   0 - raw 语义：释放锁后唤醒等待者，允许 barging
 *   1 - handoff 语义：unlock 时 owner 直接移交给队首等待者
 */

struct wait_queue {
	struct list_node head;
	uint64_t count;
	spinlock_t lock;
};

struct mutex {
	// 锁标记：0-开，1-关
	atomic_t locked;
	// 当前持有者(调试用)
	struct thread *owner;
	/* Highest-priority waiter currently donated to owner, or
	 * SCHED_PRIO_NONE when no donation is attached. */
	uint8_t donated_priority;
	// 等待队列
	struct wait_queue waiters;
};

void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
int __wake_up_all(struct wait_queue *wq);
int wake_up_all(struct wait_queue *wq);

#endif
