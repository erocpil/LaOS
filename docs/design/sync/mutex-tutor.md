# Mutex tutorial

[中文版](mutex-tutor-zh.md)

A mutex lets a thread wait for ownership instead of continuously spinning.
LaOS adds priority-ordered waiters and single-hop priority inheritance. The
[architecture note](mutex-design.md) defines the raw and handoff policies.

## 1. Choose a schedulable context

Use a mutex only where a thread may block and the owner can eventually run.
An IRQ handler cannot sleep waiting for a mutex. Holding a spinlock while
blocking prevents the progress needed to satisfy the wait.

In `kernel/lock.h`, distinguish raw spinlocks, preemption-disabling spinlocks
and IRQ-save locks. Disabling local interrupts prevents local interrupt
reentry; it does not exclude another CPU without the lock itself.

The mutex contract is therefore “sleepable thread context”. Do not call it
from an IRQ path, do not hold a spinlock that the owner needs, and do not use
interrupt masking as a replacement for the mutex.

## 2. Follow uncontended and contended acquisition

Read `mutex_lock()` and the selected policy in `kernel/mutex.c`. Fast
acquisition uses an atomic operation. The slow path retries while holding
the waiter lock before it marks current BLOCKED and schedules.

That retry closes a lost-wakeup window: an unlock between the first failed
attempt and waiter insertion must not leave the thread asleep on a free lock.

## 3. Distinguish the two policies

With `CONFIG_MUTEX_HANDOFF=0`, unlock clears ownership and wakes the best
waiter. A new arrival can acquire the free mutex before the waiter runs.
This is barging.

With `CONFIG_MUTEX_HANDOFF=1`, ownership passes to the selected waiter while
the mutex stays locked. The awakened thread recognizes itself as owner.
Within equal priorities the queue preserves FIFO order; different priorities
are selected by their effective value.

The default gate builds the raw policy. It does not validate a separate
handoff build merely because both implementations exist in the source.

## 4. Follow priority inheritance

```text
low (48) owns M
high (8) blocks on M -> low becomes effective priority 8
low unlocks M       -> donation removed -> low returns to base priority 48
```

Medium-priority work at 32 should not delay the boosted owner. If the owner
holds two contended mutexes, donation counts retain the remaining boost when
one is released.

Inheritance is single-hop. If low is already blocked on a different owner's
mutex, the new boost does not propagate through that dependency chain.
Changing a BLOCKED thread's base priority is also rejected.

## 5. Observe the existing inversion test

```sh
make test-x86_64
```

Read the `priority` result and inspect `kernel/test_priority.c`. The
`medium_before_unlock=0` check provides ordering evidence that a final
counter value alone would not provide. The restored priority checks donation
removal as well as donation creation.

When tracing the test, record these events separately: failed fast acquisition,
waiter insertion, owner donation, waiter wakeup, ownership acquisition and
donation removal. A final counter value can pass even when the ordering is
wrong; the event order is the useful evidence.

## 6. Exercise: account for two donations

An owner has base priority 48; mutex A contributes 16 and mutex B contributes
8. Compute its effective priority before and after each unlock, in both
unlock orders. Then identify the three locks in the documented order:
waiter lock, owner PI lock and owner's runqueue lock.

For read-mostly object lifetime rather than exclusive ownership, continue
with the [RCU tutorial](rcu-tutor.md).

## 7. Exercise: close the lost-wakeup window

Draw two timelines for an unlock racing with a waiter:

1. unlock occurs before the waiter retries under `waiters.lock`;
2. unlock occurs after the waiter is linked and marked BLOCKED.

For each timeline, identify who owns `waiters.lock`, which task is READY or
BLOCKED, and why the waiter cannot remain asleep while the mutex is free.
