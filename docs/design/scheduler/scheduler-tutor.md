# Scheduler tutorial

[中文版](scheduler-tutor-zh.md)

The scheduler chooses which eligible thread runs on a CPU. LaOS uses strict
fixed priorities and round-robin selection among equals. Read the
[architecture note](scheduler-arch.md) for the precise runqueue invariants.

## 1. Separate state from queue membership

Open `kernel/thread.h` and find READY, RUNNING, BLOCKED, SLEEPING and ZOMBIE.
Then inspect `pick_next()` in `kernel/sched.c`.

A linked runqueue node is not necessarily runnable. BLOCKED and SLEEPING
threads can remain linked; the selector checks their state. Idle is a
per-CPU fallback rather than an ordinary highest-priority queue entry.

This explains why waking a mutex waiter changes its state without inserting
its runqueue node a second time.

## 2. Follow priority selection

There are 64 buckets: 0 is highest and 63 lowest. A bitmap identifies nonempty
buckets; it does not eliminate state checks within a bucket.

Consider READY tasks at priorities 8, 32 and 48. The task at 8 wins. If it
remains runnable, yielding does not promise service to 48: the current RUNNING
task is explicitly compared with candidates. Equal-priority runnable tasks
rotate; lower-priority tasks can starve.

## 3. Follow a switch, not just a selection

Read `__schedule()`, address-space switching and then `switch_to()` in
`kernel/arch/x86_64/switch.asm`. The switch saves a kernel continuation and
FPU state, selects another saved stack and returns into that continuation.

For a newly created thread, a constructed frame leads to `ret_from_fork`.
That label is the first-run trampoline; it is not evidence of a fork syscall.

## 4. Follow a remote wakeup

Locate enqueue in `kernel/arch/x86_64/cpu.c`, then
`ipi_reschedule_cpu()`. Queue publication precedes the request. A local
request sets `need_resched`; a remote request also sends an interrupt.
Interrupt return or `preempt_enable()` can then act on the flag.

CPU placement is explicit. It does not imply load balancing or migration
of an already running workload to an idle CPU.

## 5. Observe the priority contract

```sh
make test-x86_64
make test-x86_64-sched-stress
```

The normal `priority` test checks high, equal-A, equal-B and promoted-low
ordering, then a mutex inversion scenario. Look for:

```text
[priority] order high=1 equal-a=2 equal-b=3 low=4
[priority] PASSED: boost=1 medium_before_unlock=0 restored=48
```

The stress target checks adjacent SMP behavior; it does not replace the
deterministic priority assertion. See the
[mutex tutorial](../sync/mutex-tutor.md) for the donation phase.

## 6. Exercise: reason about starvation

Take a permanently runnable task at priority 8 and a periodic task at 32.
Explain why round-robin within bucket 8 cannot provide a latency bound for
bucket 32. Then explain why changing the high task to SLEEPING can make the
lower task eligible without changing either priority.

## 7. Exercise: distinguish queue membership from readiness

Pick one BLOCKED or SLEEPING thread and trace its runqueue node, status field,
bucket and bitmap bit. Explain why wakeup must update the status and queue
membership exactly once, and why `need_resched` does not itself enqueue a
thread or prove that it has run.
