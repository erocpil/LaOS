# Chapter 7: Synchronization

[中文版](07-synchronization-zh.md)

> **Prerequisites**: [Scheduling](04-scheduler.md), shared memory and linked lists.
> **You'll build**: a choice between exclusion, blocking and deferred reclamation.
> **Cross-reference**: [mutex tutorial](../design/sync/mutex-tutor.md),
> [mutex architecture](../design/sync/mutex-design.md) and
> [RCU tutorial](../design/sync/rcu-tutor.md).

---

## 7.1 State the guarantee first

A shared counter, a contended resource and a removed list node need different
guarantees. The counter may need atomic updates; the resource needs ownership;
the node needs to stay alive while old readers still reference it.

| Mechanism | Main guarantee | Key restriction |
| --- | --- | --- |
| Atomic operation | one supported operation on shared state | not an entire multi-field invariant |
| Spinlock | exclusive critical section | do not block while holding it |
| Mutex | exclusive ownership with sleeping waiters | requires schedulable context |
| RCU | reader lifetime across publication/removal | writers still need their own serialization |

Choose from the required guarantee rather than from an expectation that one
primitive is always faster.

## 7.2 Local interrupt exclusion is only local

`kernel/lock.h` distinguishes raw spinlocks, preemption-disabling spinlocks
and IRQ-save locks. If an IRQ on the same CPU can acquire a lock, the interrupted
owner must prevent that reentry while holding it.

Disabling interrupts does not stop another CPU accessing the object. The
lock supplies cross-CPU exclusion; saved interrupt state controls local
reentry and must be restored rather than unconditionally enabled.

## 7.3 Sleeping introduces scheduler dependencies

A mutex waiter becomes BLOCKED and lets other work run. The slow acquisition
path rechecks ownership under its waiter lock so an unlock cannot be lost
between the failed fast attempt and sleeping.

The default raw policy permits barging after unlock. Handoff reserves
ownership for the selected waiter. Both use priority ordering and single-hop
donation to help an owner run when a higher-priority thread waits.

Donation counts aggregate several mutexes, but do not propagate through
arbitrary owner chains. That boundary matters when reasoning about nested
locks and worst-case waiting time.

## 7.4 Removal is not reclamation

An RCU reader may hold a pointer after a writer removes its node from a list.
The writer must wait until old readers finish before freeing the node:

```text
initialize -> publish -> readers may retain pointers
             remove -> synchronize_rcu -> reclaim
```

Publication ordering makes initialized fields visible. The grace period
protects lifetime. Neither operation makes arbitrary concurrent field
updates safe without a separate rule.

LaOS permits reader preemption and tracks switched-out readers as well as
per-CPU quiescent states. `synchronize_rcu()` waits in thread context. There
is no implemented `call_rcu()` callback queue; the existing design discussion
describes future work.

## Experiment: inspect ordering, not only totals

```sh
make test-x86_64
make test-x86_64-rcu-stress
```

Use the priority inversion check to explain why medium-priority work must
not run before the boosted owner unlocks. Use the RCU publication and stress
checks to separate visible initialization from delayed reclamation.

As a reading exercise, find a pointer used inside `rcu_read_lock()` and
identify its last permitted use before the matching unlock. Explain why
copying that pointer into a local variable does not extend its lifetime.

**Previous**: [Devices](06-devices.md). **Next**: [Porting](08-porting.md).
