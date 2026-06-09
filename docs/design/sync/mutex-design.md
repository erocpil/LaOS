# Mutex architecture

## Scope and state

LaOS exposes one mutex API with two compile-time policies:

- `CONFIG_MUTEX_HANDOFF=0`: raw unlock/wakeup with barging;
- `CONFIG_MUTEX_HANDOFF=1`: ownership handoff to the selected waiter.

Both use a priority-ordered wait queue and single-hop priority inheritance.
`struct mutex` contains `locked`, `owner`, `donated_priority` and a protected
wait queue.

`waiters.lock` protects the waiter list/count, owner transitions made by mutex
paths and the mutex's attached donation. Waiters are ordered by effective
priority, smallest number first. Insertion is after existing equal-priority
waiters, preserving FIFO order among equals.

Blocked threads remain linked in their CPU runqueue. `__wake_up_one()` removes
only `wait_node` and marks the selected task READY; it must not enqueue the
same runqueue node a second time.

## Donation model

Each mutex donates its highest-priority waiter's effective value. When that
value changes, the old donation is removed from the owner and the new one is
added. Each owner stores a count per priority, so several owned mutexes
aggregate correctly:

```text
base=48, mutex A donates 16, mutex B donates 8 -> effective=8
unlock B -> effective=16
unlock A -> effective=48
```

Owner attach computes a donation from existing waiters. Owner detach removes
the mutex's donation before clearing the pointer. Raw and handoff paths share
these helpers.

Lock ordering is:

```text
mutex.waiters.lock
    -> owner.pi_lock
        -> owner's runqueue.lock
```

The scheduler does not acquire these locks in reverse order.

## Raw policy

Fast acquisition uses an acquire compare/exchange. The slow path takes
`waiters.lock`, retries acquisition to close the lost-wakeup window, marks
current BLOCKED, inserts it by priority/FIFO order, refreshes donation,
releases the lock and schedules.

Unlock takes `waiters.lock`, detaches the owner/donation, publishes critical
section writes before clearing `locked`, then wakes the highest-priority
waiter. Because `locked` becomes zero before that task necessarily runs, a
newcomer may barge. PI bounds inversion while the old owner holds the mutex;
raw policy does not promise acquisition fairness after unlock.

## Handoff policy

With waiters present, unlock keeps `locked=1`, removes the old owner's
donation, wakes the highest-priority waiter and attaches it as owner. The
selected task observes `owner == current` with acquire semantics and returns
from `mutex_lock_handoff()` without competing on `locked`.

This prevents newcomer barging. Equal-priority waiters are handed off FIFO;
different priorities follow the priority queue. With no waiters, handoff
detach/publish/clear is equivalent to a normal release.

## Priority inversion

```text
low(48):    lock M ---------------- work -------- unlock M
high(8):                 lock M -> BLOCKED      acquire
medium(32):                         READY

donation: high(8) -> M -> low
effective low: 48 -> 8 -> 48
```

After high blocks, low's runqueue node migrates from bucket 48 to bucket 8,
so low runs before medium. Unlock removes the donation and migrates low back.

## Deliberate boundary

Inheritance is not transitive:

```text
high waits on M1 owned by low
low waits on M2 owned by very-low
```

High boosts low, but that change is not propagated as a revised donation to
the owner of M2. There is no deadlock-cycle detector. The accurate description
is “single-hop inheritance with multiple-mutex aggregation,” not full PI.

Changing a BLOCKED waiter's base priority is rejected because wait-queue order
and the owner's donation cannot yet be updated as one serialized operation.

## Configuration and evidence

Normal boot uses raw mutexes and disables the legacy infinite mutex stress
workers:

```c
CONFIG_MUTEX_HANDOFF=0
CONFIG_MUTEX_STRESS=0
```

The exact inversion test and adjacent SMP stress gates are:

```sh
make test-x86_64
make test-x86_64-sched-stress
make test-arm64-limine
make test-arm64-limine-sched-stress
```

The default runtime gate exercises raw policy. Handoff remains implemented but
does not have a separate hosted CI build variant.

Before adding transitive PI, the kernel needs a wait-for-chain model, bounded
propagation, cycle handling, multi-mutex lock ordering and nested-lock tests.
Before blocked reprioritization, it needs an atomic wait-queue reorder and
donation update operation.
