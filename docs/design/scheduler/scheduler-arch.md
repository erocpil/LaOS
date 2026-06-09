# Scheduler architecture

## Scope

LaOS uses a fixed-priority, per-CPU scheduler intended to make runqueue and
preemption mechanics visible. It provides:

- 64 static priority levels (`0` highest, `63` lowest);
- priority `32` as the default;
- strict selection between different priorities;
- round-robin rotation between runnable threads at the same priority;
- explicit CPU affinity, remote wakeups and reschedule IPIs;
- single-hop mutex priority inheritance.

It does not provide dynamic load balancing, time-based priority decay,
deadline scheduling, CFS-style proportional fairness or starvation
prevention.

## Source map

| File | Responsibility |
| --- | --- |
| `kernel/thread.h`, `kernel/thread.c` | priority state and mutation API |
| `kernel/sched.c` | runnable selection and context-switch policy |
| `kernel/arch/*/cpu.c` | per-CPU runqueue enqueue/dequeue |
| `kernel/arch/*/ipi.c` | local and remote reschedule requests |
| `kernel/mutex.c` | priority-ordered waiters and donation |
| `kernel/test_priority.c` | deterministic ordering and inversion gate |
| `kernel/arch/x86_64/switch.asm` | x86_64 register/FPU switch |
| `kernel/arch/aarch64/switch.S` | ARM64 register/FP-SIMD switch |

## Priority state

Each `struct thread` contains:

| Field | Meaning |
| --- | --- |
| `base_priority` | priority selected through `thread_set_priority()` |
| `priority` | effective priority used as the runqueue bucket |
| `pi_donations[64]` | count of active mutex donations at each priority |
| `pi_lock` | serializes base/effective priority changes |

The effective value is:

```text
min(base_priority, highest active donation)
```

where “highest” means the numerically smallest value. Donation counts, rather
than one saved value, let a thread own several contended mutexes: releasing
one mutex removes only that mutex's donation and preserves donations from the
others.

`thread_priority_init()` is mandatory for dynamically created threads and
architecture-owned static TCBs. `thread_set_priority()` returns `0` on
success, `-1` for invalid input, and `-2` when asked to change a blocked
waiter. Changing a blocked waiter's base priority is rejected because its
current effective value is already represented in a mutex owner's donation;
updating both atomically is not implemented.

## Per-CPU runqueue

Each CPU owns 64 list heads, a non-empty bitmap, an atomic node count and a
spinlock. A set bitmap bit means its list contains a thread. It does not mean
that the list contains a runnable thread: LaOS deliberately leaves RUNNING,
BLOCKED and SLEEPING threads linked in the runqueue. Selection must inspect
thread state after finding a non-empty bucket.

The maintained invariants are:

1. every non-idle live thread is linked at most once;
2. a linked thread is in `heads[thread->priority]`;
3. a bitmap bit is clear exactly when its list is empty;
4. `count` is the number of linked nodes, not the number of READY nodes;
5. idle is a `cpu_context` fallback, not a normal runqueue node;
6. a zombie leaves the runqueue before reusing its node in the zombie queue.

ARM64's static direct-boot and Limine idle/boot TCBs follow the same priority
initialization rule. ARM64 idle nodes are not inserted in bucket 0; doing so
would make idle the highest-priority runnable task.

## Selection algorithm

`pick_next()` runs with local interrupts disabled and the runqueue lock held.

1. Copy the non-empty bitmap.
2. Find its lowest set bit with `ctz`.
3. Scan that bucket for a READY thread or an expired SLEEPING thread.
4. If none is eligible, clear the bit only in the local bitmap copy and
   continue with the next non-empty priority.
5. Compare the candidate with the currently RUNNING non-idle thread.
6. Keep current if the candidate is absent or strictly lower priority.
7. Otherwise select the candidate and rotate its node to the bucket tail.

The explicit current-thread comparison is required because current is
RUNNING, not READY, so the normal scan skips it. Without that comparison a
high-priority current thread could be replaced by a lower-priority READY
thread.

For equal priorities, another READY thread wins and rotates to the tail.
Repeated timer preemption therefore provides round-robin service. If the
selected thread is already current, `__schedule()` returns without an
artificial context switch or RCU switch hook.

Strict priority means a continuously runnable priority-8 thread can starve a
priority-32 thread. This is policy. The legacy infinite mutex workers are
therefore opt-in through `CONFIG_MUTEX_STRESS`; normal boot does not create
one permanent default-priority worker per CPU.

## Changing the priority of a linked thread

Changing only `thread->priority` would make the bucket and TCB disagree.
`thread_apply_effective_priority()` therefore:

1. holds the per-thread PI lock with local interrupts disabled;
2. identifies the target CPU and takes its runqueue lock;
3. removes the node from the old bucket and maintains its bitmap bit;
4. changes the effective priority;
5. inserts the node at the new bucket's tail and sets its bit;
6. releases the queue and requests rescheduling on the target CPU.

An initialized but not-yet-enqueued thread has a self-linked node, so only its
TCB value changes. Enqueue later uses that value.

Lock ordering is:

```text
thread.pi_lock -> target runqueue.lock
```

The scheduler takes only `runqueue.lock` and never takes `pi_lock`.

## Wakeup and reschedule ordering

Enqueue marks a thread READY, inserts it under the target queue lock, then
calls `ipi_reschedule_cpu()`. For both architectures a local request sets
`need_resched` without sending an IPI to itself. A remote request
release-stores `need_resched` before the interrupt:

- x86_64 currently broadcasts because logical CPU IDs are not mapped back to
  arbitrary APIC IDs;
- ARM64 sends directed SGI 3 for the QEMU `virt` affinity layout.

Interrupt return and `preempt_enable()` acquire-observe `need_resched`.
Consequently a local enqueue is not dependent on a later unrelated timer tick.

## Context-switch layout dependency

Architecture switch code uses generated offsets into `struct thread`,
especially `THREAD_FPU_STATE`. Extending the TCB without rebuilding assembly
makes FPU save/restore write to an old offset and corrupt later fields such as
the thread name and runqueue node.

`script/gen_offsets.sh` changes generated files only when contents change.
`kernel.mk` explicitly makes x86_64 NASM switch/syscall objects depend on
`asm_offsets_nasm.inc`; ARM64 `.S` dependencies use compiler-generated `.d`
files. Both ARM64 direct and Limine paths regenerate offsets before compiling.
This dependency is part of the TCB ABI: incremental builds must be correct,
not only clean builds.

## Test contract

Enable the built-in test with:

```text
@test priority timeout_ticks=500
```

The ordering phase enqueues low (`48`), equal A (`32`), equal B (`32`) and
high (`8`). It requires high, A and B in that order, proves low does not run
while higher work remains, then promotes the already-linked low thread to
priority `8` and requires it fourth. This also exercises bucket migration.

The inversion phase has low acquire a mutex and lower its base priority to
`48`. Medium (`32`) and high (`8`) become runnable; high blocks on the mutex
and donates `8`. Low must run before medium, observe the boost, unlock and
return to `48`. High then acquires/releases the mutex and medium eventually
runs.

Canonical detail:

```text
[priority] order high=1 equal-a=2 equal-b=3 low=4
[priority] PASSED: boost=1 medium_before_unlock=0 restored=48
[selftest] 'priority' PASSED
```

Maintained evidence:

```sh
make test-x86_64
make test-x86_64-sched-stress
make test-arm64
make test-arm64-limine
make test-arm64-limine-sched-stress
```

Normal x86_64 and ARM64 Limine gates require the priority marker. Direct ARM64
covers static-TCB/idle fallback construction; stress targets cover adjacent
SMP scheduling paths.

## Known limits and safe evolution

- priorities are kernel-internal; task configuration and user syscalls do not
  assign them;
- there is no aging, bandwidth control, starvation detector or load balancing;
- bitmap lookup is O(1), but state filtering within a bucket is linear;
- mutex inheritance is single-hop, not a transitive dependency graph;
- changing a BLOCKED thread's base priority is unsupported;
- CPU hotplug is absent.

A useful next step is bounded starvation/latency instrumentation or explicit
task priority configuration. Transitive PI should come only with a wait-for
chain model, cycle handling and a dedicated nested-lock test.
