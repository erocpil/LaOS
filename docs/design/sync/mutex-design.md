# Mutex architecture

[中文版](mutex-design-zh.md)

Start with the [mutex tutorial](mutex-tutor.md) for acquisition and donation
examples. [Chapter 7](../../book/07-synchronization.md) compares mutexes with
spinlocks and RCU.

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

## API and execution-context contract

`mutex_lock()` is a sleeping operation. It is valid only in task context where
the current thread may block and the owner can continue to run. It must not be
called from an IRQ handler, while holding a spinlock needed by the owner, or
with local interrupts disabled as a substitute for mutual exclusion.

| Operation/path | May schedule | Local interrupt rule | Main protection |
| --- | --- | --- | --- |
| fast `mutex_lock()` | no | caller state is preserved | atomic lock/owner update |
| contended `mutex_lock()` | yes | wait-queue lock is acquired with IRQ save | `waiters.lock` and queue membership |
| `mutex_unlock()` | no | wait-queue lock is acquired with IRQ save | owner, donation and waiter transition |
| `mutex_lock_handoff()` confirmation | no | caller state is preserved | acquire observation of `owner` |
| PI refresh | no | runs under the mutex path's lock order | owner PI counts and runqueue migration |

The current implementation expects the owner to unlock the mutex it owns. It
does not provide recursive locking, lock-order validation, deadlock detection,
or an IRQ-safe mutex variant. A blocked waiter remains a scheduler-managed
thread; removing it from a mutex wait queue is separate from removing its
runqueue node.

## State transitions

The common contended path has these states:

```text
free
  | atomic acquisition
  v
owned(owner=current)
  | another thread fails acquisition
  v
waiter queued, current=BLOCKED, donation refreshed
  | owner wakes one waiter
  +------------------------------+
  | raw                           | handoff
  v                               v
free + selected waiter READY      owned(owner=selected waiter)
```

In raw mode the selected waiter must compete again after it runs. In handoff
mode the lock remains logically held and the selected waiter consumes the
reserved ownership. Both paths remove the selected wait node exactly once.

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

The owner PI count is the aggregation invariant: for each priority, the count
equals the number of owned mutexes currently contributing that priority. A
mutex donation is removed before its owner pointer is cleared, and a refreshed
donation is attached only after the new owner is known. This ordering prevents
an unlock from temporarily losing a donation belonging to another mutex.

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

The lost-wakeup closure is the critical ordering rule:

```text
waiter: failed fast path
waiter: lock waiters.lock, retry acquisition
waiter: mark BLOCKED and link wait_node
waiter: refresh donation, unlock waiters.lock, schedule

owner:  lock waiters.lock, detach/release, wake selected waiter
owner:  unlock waiters.lock
```

If the owner releases between the first failed attempt and the second attempt,
the waiter acquires the mutex instead of sleeping. If release occurs after the
waiter is linked, the same `waiters.lock` serializes the wakeup with queue
membership.

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

## Evidence mapping

| Evidence | Exercises | Does not prove |
| --- | --- | --- |
| `priority` selftest | inversion ordering, donation creation/removal, effective priority restoration | transitive PI or deadlock-cycle handling |
| scheduler stress | wakeup, runqueue membership and priority changes under load | fairness of every raw-mode interleaving |
| normal x86_64/ARM64 gates | integration of the shared mutex paths with boot and scheduling | an independent handoff build |
| source lock-order review | `waiters.lock -> owner.pi_lock -> runqueue.lock` discipline | absence of all future lock-order cycles |
