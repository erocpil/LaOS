# Chapter 4: Scheduling

[中文版](04-scheduler-zh.md)

> **Prerequisites**: [Chapter 3](03-interrupt.md), linked lists and thread state.
> **You'll build**: an explanation of selection, context switching and wakeup.
> **Cross-reference**: [scheduler tutorial](../design/scheduler/scheduler-tutor.md)
> and [scheduler architecture](../design/scheduler/scheduler-arch.md).

---

## 4.1 Policy starts with eligibility

Each CPU has 64 priority buckets and a bitmap of nonempty buckets. Priority
0 is highest; 32 is the default. A thread's presence in a bucket does not
mean it may run: BLOCKED and SLEEPING nodes remain linked too.

`pick_next()` scans eligible states within the best nonempty buckets. It
also compares the selected candidate with the current RUNNING thread, which
the READY scan would otherwise skip. Idle is used when ordinary work cannot
run.

## 4.2 Strict priority and round-robin answer different questions

Priority selects between buckets. Rotation selects among equal-priority
runnable threads. A permanent priority-8 workload can prevent priority-32
work from running indefinitely; round-robin within bucket 8 does not cure
that starvation.

`yield()` therefore does not promise that every other task receives service.
Sleeping or blocking can change eligibility; yielding alone need not make
lower-priority work preferable.

## 4.3 A switch resumes a continuation

`__schedule()` updates state and selects the next task. Address-space switching
establishes the appropriate root. `switch_to()` saves the current kernel
continuation, including its stack pointer and FPU state, then restores another.

The restored return address may lead into a previously suspended scheduler
call. For a new thread, a constructed frame leads into its first-run
trampoline. The label `ret_from_fork` names that trampoline, even though no
fork syscall is implemented.

The frame offsets used by assembly are part of the C/assembly contract.
Extending a TCB without regenerating offsets can corrupt another field while
saving FPU state. This is why incremental build dependencies matter here.

## 4.4 Wakeup must reach the target CPU

Enqueue publishes the node and requests rescheduling. A remote request also
sends an IPI so an idle or busy target need not wait for an unrelated event.
The local path sets the pending flag without interrupting itself.

Waking a blocked mutex waiter differs from creating a new task: its runqueue
node is already linked. Reinserting it would damage list membership.

## 4.5 Priority inheritance changes the effective bucket

A high-priority waiter can donate priority to a lower-priority mutex owner.
The owner's node must move to the matching bucket when its effective priority
changes. On unlock, removing a donation can move it back.

This is single-hop inheritance, with donation counts for several owned
mutexes. It does not propagate through an arbitrary chain of blocked owners.
See [Chapter 7](07-synchronization.md) for the lock and lifetime contracts.

## Experiment: connect ordering to policy

```sh
make test-x86_64
make test-x86_64-sched-stress
```

Inspect the `priority` test's ordered high/equal/low phase and its inversion
phase. Identify the assertion that would fail if the boosted owner stayed
in its old runqueue bucket. Keep this deterministic evidence separate from
the stress target's broader scheduling activity.

**Previous**: [Interrupts](03-interrupt.md). **Next**: [User mode](05-user-mode.md).
