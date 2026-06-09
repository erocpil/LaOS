/*
 * mutex.c - mutex/rwlock 同步原语
 */

#include "mutex.h"
#include "debug.h"
#include "string.h"
#include "sched.h"
#include "thread.h"
#include "arch_x86.h"
#include "arch_barrier.h"

/* 调试宏：设为 #if 0 可关闭 LM / mutex_waiters 日志输出 */
#if 1
#define LM(fmt, ...)
#define mutex_waiters(m)
#else
#define LM(fmt, ...) \
	do { \
		struct thread *curr = get_current(); \
		if (!curr) { \
			panic(); \
		} \
		kprintf("[%s %d %lu CPU#%d %s %ld] " fmt "\n", \
				__func__, __LINE__, rdtsc(), \
				curr->target_cpu, curr->name, curr->id, ##__VA_ARGS__);  \
	} while (0)

#define mutex_waiters(m) \
	do { \
		__typeof__(m) _m = (m); \
		LM("Mutex waiters %ld", _m->waiters.count); \
		struct thread *pos; \
		list_for_each_entry(pos, &_m->waiters.head, wait_node) { \
			L("  %s %ld %d", pos->name, pos->id, pos->target_cpu); \
		} \
	} while (0)

#endif

void mutex_check_owner(struct mutex *m)
{
	if (m->owner != get_current()) {
		struct thread *t1 = m->owner;
		struct thread *t2 = get_current();
		L("%s %ld <> %s %ld", t1->name, t1->id, t2->name, t2->id);
	}
	WARN_ON(m->owner && m->owner != get_current());
}

#define atomic_try_lock(v) ( { \
		int expected = 0; \
		!!__atomic_compare_exchange_n(&(v)->counter, &expected, 1, \
				0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); \
				} )

/* wait_queue helpers， 内部调用时已持有 wq->lock */
int __wake_up_one(struct wait_queue *wq)
{
	/* 无人等待 */
	if (list_empty(&wq->head)) {
		return 0;
	}

	/*
	 * 取等待队列首线程，脱链，改 READY.
	 * 不调 cpu_enqueue:线程在 LaOS 中 BLOCKED 后仍留在 run queue 上，
	 * pick_next 按状态筛选，改为 READY 即可被调度器选中。
	 */
	struct thread *t = list_first_entry(&wq->head, struct thread, wait_node);

	list_del_init(&t->wait_node);
	wq->count--;
	thread_set_status(t, THREAD_READY);

	return 1;
}

int wake_up_one(struct wait_queue *wq)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&wq->lock, flags);

	int n = __wake_up_one(wq);

	arch_spin_unlock_irqrestore(&wq->lock, flags);

	return n;
}

int __wake_up_all(struct wait_queue *wq)
{
	int n = 0;

	while (!list_empty(&wq->head)) {

		struct thread *t = list_first_entry(&wq->head, struct thread, wait_node);

		list_del_init(&t->wait_node);

		wq->count--;

		n++;

		LM("leave mutex to %s %ld", t->name, t->id);
		thread_set_status(t, THREAD_READY);
		cpu_enqueue(t->target_cpu, t);
		smp_mb();
	}

	return n;
}

int wake_up_all(struct wait_queue *wq)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&wq->lock, flags);

	int n = __wake_up_all(wq);

	/* 防御性检查:list 已空但 count 不为 0 说明维护出了 bug */
	if (wq->count) {
		panic("wait queue count %ld", wq->count);
	}

	arch_spin_unlock_irqrestore(&wq->lock, flags);

	return n;
}

void mutex_init(struct mutex *m)
{
	memset(m, 0, sizeof(struct mutex));

	atomic_set(&m->locked, 0);

	spin_lock_init(&m->waiters.lock);

	list_init(&m->waiters.head);

	m->donated_priority = SCHED_PRIO_NONE;
	__atomic_store_n(&m->owner, NULL, __ATOMIC_RELEASE);
}

static int mutex_waiter_priority_locked(struct mutex *m)
{
	if (list_empty(&m->waiters.head))
		return SCHED_PRIO_NONE;

	return list_first_entry(&m->waiters.head,
			struct thread, wait_node)->priority;
}

/* Keep FIFO order among equal priorities while placing the numerically
 * smallest (highest) priority at the front for handoff and donation. */
static void mutex_add_waiter_locked(struct mutex *m, struct thread *t)
{
	struct thread *pos;

	list_for_each_entry(pos, &m->waiters.head, wait_node) {
		if (t->priority < pos->priority) {
			__list_add(&t->wait_node, pos->wait_node.prev,
					&pos->wait_node);
			return;
		}
	}
	list_add_tail(&t->wait_node, &m->waiters.head);
}

static void mutex_refresh_donation_locked(struct mutex *m)
{
	struct thread *owner = __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE);
	int new_priority = owner ? mutex_waiter_priority_locked(m) :
		SCHED_PRIO_NONE;
	int old_priority = m->donated_priority;

	if (old_priority == new_priority)
		return;
	if (owner)
		thread_priority_update_donation(owner, old_priority,
				new_priority);
	m->donated_priority = (uint8_t)new_priority;
}

static void mutex_attach_owner_locked(struct mutex *m, struct thread *owner)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	struct thread *previous = m->owner;
	if (previous) {
		LM("MUTEX CORRUPTION: owner is %s %ld@%d <> but %s %ld@%d is trying to take it!",
				previous->name, previous->id, previous->target_cpu,
				owner->name, owner->id, owner->target_cpu);
		WARN_ON(m->owner);
	}

	m->donated_priority = SCHED_PRIO_NONE;
	__atomic_store_n(&m->owner, owner, __ATOMIC_RELEASE);
	mutex_refresh_donation_locked(m);
}

static void mutex_detach_owner_locked(struct mutex *m)
{
	struct thread *owner = __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE);

	if (owner && m->donated_priority < SCHED_PRIO_COUNT)
		thread_priority_update_donation(owner, m->donated_priority,
				SCHED_PRIO_NONE);
	m->donated_priority = SCHED_PRIO_NONE;
	__atomic_store_n(&m->owner, NULL, __ATOMIC_RELEASE);
}

/**
 * mutex_lock_handoff() / mutex_unlock_handoff()
 *
 * handoff 语义：unlock 时不释放 locked，直接将 owner 指针移交给队首等待者。
 * 新来者看到 owner ！= NULL 时 fast path 短路，必须排队。
 * 被 handoff 的线程通过 m->owner == current 识别到锁已预留给自己。
 */
void mutex_lock_handoff(struct mutex *m)
{
	struct thread *current = get_current();

	/*
	 * 第一优先级：handoff 确认。
	 * 如果 owner 已经是自己，说明 unlock 已把锁移交过来。
	 * acquire 语义确保看到 unlock 路径的所有内存修改。
	 */
	if (likely(__atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == current)) {
		LM("Mutex(%s) handoff confirmed %s %ld %d",
				m->owner->name,
				current->name, current->id, current->target_cpu);
		return;
	}

	/*
	 * 第二优先级：fast path。
	 * 仅在 owner 为空时尝试 cmpxchg，防止破坏他人的 handoff.
	 */
	if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == NULL && atomic_try_lock(&m->locked)) {
		/*
		 * acquire barrier：cmpxchg 在 x86 上自带 LOCK 前缀隐含 full barrier，
		 * 但在弱序架构(ARM/RISC-V)上需要显式 acquire 语义，
		 * 确保临界区内的读写不会被重排到加锁之前。
		 */
		smp_mb__after_atomic();
		uint64_t flags = 0;
		arch_spin_lock_irqsave(&m->waiters.lock, flags);
		mutex_attach_owner_locked(m, current);
		arch_spin_unlock_irqrestore(&m->waiters.lock, flags);
		LM("Mutex(%s) tried %s %ld %d", m->owner->name,
				current->name, current->id, current->target_cpu);
		return;
	}

	/*
	 * slow path：持锁者持有锁，将自己加入等待队列后调度出去。
	 * 持有 waiters.lock 期间再次尝试获取，避免与 mutex_unlock
	 * 的唤醒操作之间产生竞争窗口(unlock先清锁再唤醒的间隙).
	 */
	while (1) {
		if (likely(__atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == current)) {
			LM("Mutex(%s) handoff reload confirmed %s %ld %d", m->owner->name,
					current->name, current->id, current->target_cpu);
			return;
		}
		L("CPU locking %d", cpu_get_ctx()->id);

		uint64_t flags = 0;
		arch_spin_lock_irqsave(&m->waiters.lock, flags);

		if (__atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == current) {
			LM("Mutex(%s) handoff reload confirmed %s %ld %d",
					m->owner->name,
					current->name, current->id, current->target_cpu);
			arch_spin_unlock_irqrestore(&m->waiters.lock, flags);
			return;
		}
		/*
		 * double check：持锁者可能在我们拿到 waiters.lock 之前已经释放，
		 * 需要再次尝试获取以防止 lost wakeup。
		 */
		if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == NULL && atomic_try_lock(&m->locked)) {
			smp_mb__after_atomic();
			mutex_attach_owner_locked(m, current);
			LM("Mutex(%s) double %s %ld %d",
					m->owner->name,
					current->name, current->id, current->target_cpu);

			arch_spin_unlock_irqrestore(&m->waiters.lock, flags);

			return;
		}

		/*
		 * enqueue current task。
		 * 检查是否已在队列中(虚假唤醒后重试时可能已在队列里)，
		 * list_del_init 配合此检查防止重复加入。
		 */
		if (list_empty(&current->wait_node)) {
			thread_set_status(current, THREAD_BLOCKED);
			mutex_add_waiter_locked(m, current);
			m->waiters.count++;
			mutex_refresh_donation_locked(m);
			smp_mb();
		}
		current->sleep_times++;

		/*
		 * 关键顺序：必须在 unlock spinlock 之前将自己加入等待队列，
		 * 否则 mutex_unlock 可能在我们加入队列之前就唤醒了(空唤醒)，
		 * 导致当前线程永久睡眠。
		 * arch_spin_unlock_irqrestore 自带 release barrier，
		 * 保证上面的链表操作对其他CPU可见后才释放 spinlock。
		 */
		arch_spin_unlock_irqrestore(&m->waiters.lock, flags);

		/*
		 * schedule() 让出CPU.被唤醒后从这里继续，
		 * 重新进入 while(1) 顶部竞争锁。
		 */
		LM("Mutex(%s) scheduling %s %ld %d",
				m->owner->name,
				current->name, current->id, current->target_cpu);

		schedule();
	}
}

void mutex_unlock_handoff(struct mutex *m)
{
	/* 防御性检查：只有owner才能解锁 */
	mutex_check_owner(m);

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&m->waiters.lock, flags);

	/*
	 * 顺序修正：原代码先 atomic_set locked=0 再唤醒，存在 barging 窗口：
	 *   T_unlock: locked=0
	 *   T_new:    cmpxchg(0->1) 成功，拿走锁       <- 插队
	 *   T_unlock: wake_up_one(T_waiter)
	 *   T_waiter: 醒来，cmpxchg 失败，再次睡眠    <- 虚假唤醒
	 *
	 * handoff 策略：持有 waiters.lock 时检查是否有等待者：
	 *   - 有等待者：直接将锁"移交"给被唤醒线程(locked 保持1)
	 *   - 无等待者：释放锁(locked=0)
	 */

	int n = 0;
	if (!list_empty(&m->waiters.head)) {
		/*
		 * handoff 语义：直接将 owner 移交给队首等待者，locked 保持 1，
		 * 防止新来者插队(barging)。被唤醒线程在 mutex_lock_handoff
		 * 中通过 m->owner == current 识别到锁已预留给自己。
		 */
		struct thread *next = list_first_entry(&m->waiters.head,
				struct thread, wait_node);
		mutex_detach_owner_locked(m);
		LM("Mutex(%s) handoff to %s %ld %d",
				get_current()->name, next->name, next->id, next->target_cpu);
		n = __wake_up_one(&m->waiters);
		mutex_attach_owner_locked(m, next);
	} else {
		/*
		 * release barrier: 保证临界区内所有写操作在 locked=0
		 * 对其他CPU可见之前已经完成。
		 * 在弱序架构上 atomic_set 不隐含 barrier，需要显式添加。
		 */
		mutex_detach_owner_locked(m);
		smp_mb();
		atomic_set(&m->locked, 0);
		LM("Mutex released");
	}

	(void)n;
	arch_spin_unlock_irqrestore(&m->waiters.lock, flags);
}

/**
 * mutex_lock_raw()
 *
 * raw 语义：释放锁后唤醒等待者，新来者可与被唤醒线程竞争(barging)。
 * 实现简单，吞吐量高，但等待者可能饥饿。
 */
void mutex_lock_raw(struct mutex *m)
{
	while (1) {
		/* fast path */
		if (atomic_try_lock(&m->locked)) {
			smp_mb__after_atomic();
			uint64_t flags = 0;
			arch_spin_lock_irqsave(&m->waiters.lock, flags);
			mutex_attach_owner_locked(m, get_current());
			arch_spin_unlock_irqrestore(&m->waiters.lock, flags);
			return;
		}

		/*
		 * slow path：持锁者持有锁，将自己加入等待队列后调度出去。
		 * 持有 waiters.lock 期间再次尝试获取，避免与 mutex_unlock
		 * 的唤醒操作之间产生竞争窗口(unlock先清锁再唤醒的间隙)。
		 */
		uint64_t flags = 0;
		arch_spin_lock_irqsave(&m->waiters.lock, flags);

		/*
		 * double check:持锁者可能在我们拿到 waiters.lock 之前已经释放，
		 * 需要再次尝试获取以防止 lost wakeup。
		 */
		if (atomic_try_lock(&m->locked)) {
			smp_mb__after_atomic();
			struct thread *t = get_current();
			mutex_attach_owner_locked(m, t);
			arch_spin_unlock_irqrestore(&m->waiters.lock, flags);

			return;
		}

		struct thread *current = get_current();

		/*
		 * enqueue current task。
		 * 检查是否已在队列中(虚假唤醒后重试时可能已在队列里)，
		 * list_del_init 配合此检查防止重复加入。
		 */
		if (list_empty(&current->wait_node)) {
			thread_set_status(current, THREAD_BLOCKED);
			mutex_add_waiter_locked(m, current);
			m->waiters.count++;
			mutex_refresh_donation_locked(m);
			smp_mb();
		}
		current->sleep_times++;

		arch_spin_unlock_irqrestore(&m->waiters.lock, flags);

		schedule();
	}
}

/**
 * mutex_unlock_raw()
 *
 * raw 语义：释放锁后唤醒等待者，新来者可与被唤醒线程竞争(barging)。
 * 实现简单，吞吐量高，但等待者可能饥饿。
 */
void mutex_unlock_raw(struct mutex *m)
{
	/* 防御性检查：只有owner才能解锁 */
	mutex_check_owner(m);

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&m->waiters.lock, flags);

	/*
	 * 顺序修正：原代码先 atomic_set locked=0 再唤醒，存在 barging 窗口：
	 *   T_unlock: locked=0
	 *   T_new:    cmpxchg(0->1) 成功，拿走锁       <- 插队
	 *   T_unlock: wake_up_one(T_waiter)
	 *   T_waiter: 醒来，cmpxchg 失败，再次睡眠    <- 虚假唤醒
	 *
	 * raw 策略：在 waiters.lock 保护下，先清 owner，加屏障，再清 locked，
	 * 然后唤醒等待者。新来者可以公平竞争 locked.
	 */
	int n = 0;
	mutex_detach_owner_locked(m);

	/*
	 * 内存屏障，确保 owner=NULL 写入对其他 CPU 可见。
	 * 在 x86 下，atomic 操作自带 barrier，但在非原子写入后需要 smp_mb。
	 */
	smp_mb();

	atomic_set(&m->locked, 0);

	if (!list_empty(&m->waiters.head)) {
		n = __wake_up_one(&m->waiters);
	}

	(void)n;

	arch_spin_unlock_irqrestore(&m->waiters.lock, flags);
}

void mutex_lock(struct mutex *m)
{
#if CONFIG_MUTEX_HANDOFF
	mutex_lock_handoff(m);
#else
	mutex_lock_raw(m);
#endif
}

void mutex_unlock(struct mutex *m)
{
#if CONFIG_MUTEX_HANDOFF
	mutex_unlock_handoff(m);
#else
	mutex_unlock_raw(m);
#endif
}
