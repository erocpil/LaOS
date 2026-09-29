# RCU architecture

[中文版](rcu-arch-zh.md)

## Scope

LaOS implements a compact preemptible RCU grace-period engine in shared kernel
code.  Its supported use is:

- task-context readers;
- a stable set of online CPUs;
- synchronous updater waits;
- controlled teaching and stress fixtures.

The implementation is not yet a general RCU-protected container library.
The existing forward-link publication and traversal primitives use
release/acquire operations; their architecture evidence is described below.

## State ownership

Global state in `kernel/rcu.c`:

| Field | Purpose | Access rule |
| --- | --- | --- |
| `gp_seq` | current grace-period generation | atomic operations |
| `blocked_tasks` | preempted threads still inside read sections | hold `blocked_lock` |
| `blocked_lock` | protects blocked list | local interrupt ordering follows caller contract |

Per-CPU state:

| Field | Purpose |
| --- | --- |
| `rcu_gp_seq_seen` | newest generation observed at a quiescent state |

Per-thread state:

| Field | Purpose |
| --- | --- |
| `rcu_nesting` | nested read-section depth |
| `rcu_blocked` | thread is present on global blocked list |
| `rcu_blocked_node` | intrusive blocked-list link |

Metrics in `rcu_metric` expose generation, wait count and last grace-period
cycle count to the statistics monitor.  Metrics are observability data, not
synchronisation state.

## Reader state machine

```text
nesting 0
   |
   | rcu_read_lock
   v
nesting 1..N
   |
   | context switch while nesting > 0
   v
blocked=true, node linked
   |
   | reschedule and outermost unlock
   v
node unlinked, blocked=false, nesting=0,
CPU sequence updated
```

Only the CPU currently executing a thread changes that thread's nesting.
The scheduler observes the outgoing thread before changing `current`.

Current code assumes correct callers.  It does not reject unlock underflow,
thread destruction while blocked, or an unbalanced critical section.

## API and execution-context contract

The public API is intentionally small.  The following table describes the
current implementation rather than a future Linux-compatible interface.

| Operation | Allowed context | Scheduling | Local interrupt rule | Lock rule |
| --- | --- | --- | --- | --- |
| `rcu_read_lock()` | task context only | must not schedule as part of the call | does not disable interrupts | does not acquire an RCU lock |
| `rcu_read_unlock()` | the task that entered the section | does not schedule as part of the call | disables interrupts only while removing a blocked task | must not be called after task destruction or an unmatched lock |
| `synchronize_rcu()` | schedulable task context | calls `schedule()` while waiting | follows the caller/scheduler path | caller must not hold a lock needed by an active reader |
| `rcu_note_context_switch()` | scheduler switch path | called before the switch | caller has local interrupts disabled | takes `blocked_lock` under that contract |
| `rcu_check_quiescent_state()` | timer tick path | does not schedule | runs before interrupt completion | does not take `blocked_lock` |

Nested read-side sections are supported through `rcu_nesting`, but only the
outermost unlock can remove a blocked record or publish a CPU quiescent state.
The API does not provide an IRQ or NMI read-side variant.

## Scheduler and timer hooks

`kernel/timer.c` calls `rcu_check_quiescent_state()` once per timer tick before
interrupt completion.  It updates only the current CPU's sequence and returns
early before the thread subsystem has a current task.

`kernel/sched.c` calls `rcu_note_context_switch(prev)`:

- after deciding that `prev` will be switched out;
- while local interrupts are disabled;
- before replacing the CPU's current-thread pointer;
- before the architecture context switch.

If `prev` is not reading, the switch records a quiescent state.  If it is
reading, the hook links it to `blocked_tasks` once.

The hook's raw spinlock use depends on the scheduler's local-interrupt-disabled
entry contract.  `rcu_read_unlock()` disables local interrupts before taking
the same lock, keeping lock ordering consistent.

## Grace-period algorithm

`synchronize_rcu()` atomically increments `gp_seq` and saves the returned
target generation.

Stage one visits the current `g_cpu_contexts` entries and waits until each
participating CPU has `rcu_gp_seq_seen >= target`.  A tick, a non-reader
context switch, or an outer reader unlock can advance that value.

Stage two repeatedly checks `blocked_tasks` under its lock and schedules until
the list is empty.

After both stages, any read-side critical section that began before the target
generation has either:

- exited while running;
- passed through a CPU quiescent state; or
- been tracked as blocked and later unlocked.

The safety invariant is the combination of the two observations above, not
either observation alone:

1. a CPU sequence update is valid only when its current thread is not inside a
   read-side critical section;
2. a reader that is switched out while nested is non-quiescent until its outer
   unlock removes it from `blocked_tasks`;
3. `synchronize_rcu()` returns only after every participating CPU has crossed
   the target sequence and the blocked list is empty.

The implementation does not snapshot a generation in each reader.  It instead
uses the scheduler switch hook to transfer a preempted reader from CPU-local
progress tracking to the global blocked list.  This is why the scheduler hook,
the blocked-list lock and the outermost unlock are part of the grace-period
correctness boundary.

The implementation assumes:

- `g_cpu_count` and its context pointers are stable for the whole wait;
- every counted CPU continues taking ticks or scheduling;
- the caller can schedule;
- no caller holds a lock required by a reader;
- sequence comparison does not cross an integer wrap boundary;
- updater concurrency does not rely on an undocumented ordering.

There is no timeout.  Violating a progress assumption can block indefinitely.

The implemented control flow is equivalent to:

```text
target = atomic_increment(gp_seq)

for each stable CPU context:
    while cpu.rcu_gp_seq_seen < target:
        schedule()

repeat:
    lock(blocked_lock)
    done = blocked_tasks is empty
    unlock(blocked_lock)
    if done:
        return
    schedule()
```

The comparison is an ordinary monotonic integer comparison; it is valid only
while the sequence does not wrap.  The wait is conservative: a reader that
starts after `target` may still delay return if it is observed as a current
reader or is later placed on `blocked_tasks`.  This does not weaken safety, but
it means the implementation has no expedited or reader-filtered grace period.

### Representative race boundaries

The important interleavings are:

1. **Reader before generation increment.** A reader that is still running
   prevents its CPU from publishing the target sequence until it unlocks or is
   switched out.  If it is switched out while nested, the scheduler records it
   on `blocked_tasks`.
2. **Reader preempted before stage one observes the CPU.** The CPU may later
   report progress after running another task, but the blocked reader remains
   independently visible in `blocked_tasks`; stage two prevents premature
   reclamation.
3. **Reader after generation increment.** It is not required by the grace
   period, but the current global blocked list is not generation-tagged.  If it
   becomes blocked before the updater checks the list, the updater may wait for
   it as well.
4. **Outermost unlock racing the updater.** The unlock removes the blocked node
   while holding `blocked_lock`; the updater checks the same list under that
   lock.  It therefore observes either the still-linked reader or the completed
   removal, never a partially modified list.

These boundaries explain why the implementation relies on scheduler placement
and lock ordering, not only on atomic operations around `gp_seq`.

## Publication and reclamation

The grace-period engine answers when old readers have completed.  Container
code must separately provide:

- writer-to-writer exclusion;
- safe object initialisation and publication;
- reader acquire semantics;
- unlink without destroying traversal state;
- reclamation only after the grace period.

The RCU helpers in `kernel/list.h` leave removed links intact and use
release-store/acquire-load operations for forward-link publication/traversal.

The list API has a deliberately narrow contract:

- only forward `next` traversal is RCU-safe;
- `prev` is maintained for writers and is not a reverse-traversal guarantee;
- writers must serialize their updates independently of RCU;
- `list_first_entry_rcu()` requires a non-empty list; a null-returning helper is
  not implemented;
- a reader must not retain a node pointer after the outermost unlock.

Current portability boundary:

| Concern | x86_64 | ARM64 |
| --- | --- | --- |
| grace-period atomic generation | compiler atomic primitives | compiler atomic primitives |
| scheduler/timer hooks | wired | shared hooks compile and are wired through common code |
| list publication ordering | release/acquire (normally plain `mov`) | release/acquire (`STLR`/`LDAR`) |
| bounded list evidence | `rcu_publish` selftest | selftest plus instruction-shape gate |

The list contract is now architecture-neutral; architecture-specific evidence
belongs in build gates rather than in caller-side assumptions.

## Planned asynchronous callback architecture

This section is a design contract, not a description of code that already
exists.  LaOS currently exposes only `synchronize_rcu()` and has no
`struct rcu_head`, callback queue, worker or `rcu_barrier()`.

### API and ownership

The proposed public record is an intrusive queue node:

```c
struct rcu_head {
	struct rcu_head *next;
	void (*func)(struct rcu_head *head);
};

void call_rcu(struct rcu_head *head,
	      void (*func)(struct rcu_head *head));
void rcu_barrier(void);
```

The caller embeds `rcu_head` in the reclaimable object, removes that object
from every RCU-published structure, and then calls `call_rcu()`.  Successful
submission transfers temporary ownership of the record and enclosing
object to RCU until callback completion.  Double submission of one record is
invalid and should be detected in debug builds.

`call_rcu()` guarantees that `func(head)` runs only after all read-side
critical sections that could predate the submission have completed.  It does
not guarantee that no new readers are running when the callback executes.

### Queue state and batch boundary

The first implementation should prefer explicit state over a highly scalable
Linux-style per-CPU callback hierarchy:

| State | Meaning |
| --- | --- |
| `pending` | submitted callbacks not assigned to a started grace period |
| worker-local `batch` | callbacks sealed before the worker starts its grace period |
| `executing` | grace period completed; callback currently owns the record |

The queue needs a dedicated lock rather than reusing `blocked_lock`.
`blocked_lock` protects preempted-reader state and is taken from scheduler and
outer-unlock paths; mixing callback submission into that lock would create
unnecessary ordering and interrupt-context dependencies.

The essential rule is that the worker detaches a fixed batch before starting
its grace period:

```text
submit A, B, C
        |
worker: detach pending -> local batch
        |
worker: synchronize_rcu()
        |                       submit D -> pending for next batch
        v
execute A, B, C
        |
next grace period may cover D
```

A callback submitted while a grace period is already in progress must not be
blindly appended to that period's completed batch.  The grace period might
have started before the corresponding object was unlinked, allowing a reader
to acquire the object after the grace period began.  Keeping new submissions
in `pending` for the next grace period is the simplest safe rule.  A later
generation-tagged design may coalesce more aggressively only if it proves the
same coverage.

### Submission and worker paths

Conceptual submission:

```c
void call_rcu(struct rcu_head *head, rcu_callback_t func)
{
	head->func = func;

	spin_lock(&callback_lock);
	append_fifo(&pending, head);
	wake_rcu_worker();
	spin_unlock(&callback_lock);
}
```

Conceptual worker:

```c
for (;;) {
	wait_for_callbacks_or_shutdown();

	spin_lock(&callback_lock);
	batch = detach_pending();
	spin_unlock(&callback_lock);

	if (!batch.empty)
		synchronize_rcu();

	while ((head = pop_front(&batch)) != NULL)
		head->func(head);
}
```

The real implementation must make the queue publication and wakeup atomic
with respect to the worker's sleep decision so that no callback is stranded
by a lost wakeup.  It must not hold `callback_lock` across
`synchronize_rcu()` or callback invocation.  Either operation can schedule,
take unrelated locks, enqueue another callback or run for an unbounded time.

A single global FIFO is adequate for the teaching implementation and makes
same-queue callback order explicit.  Per-CPU queues can be considered only
after the ordering, draining and hotplug contracts are tested.

### Execution-context contract

The initial LaOS worker should be an ordinary schedulable kernel thread
because the existing `synchronize_rcu()` calls `schedule()`.  The callback
contract still needs an explicit decision:

- Linux-compatible callbacks may execute with bottom halves disabled and
  therefore cannot block.
- A LaOS-specific worker-only contract could permit blocking, but callers
  must then not assume Linux callback semantics.

Keeping callbacks short and non-blocking is the conservative initial rule.
Callbacks that need to sleep should enqueue separate thread-context work.
Callback invocation must occur without internal RCU locks held.

Submission context is a separate contract.  If `call_rcu()` is allowed from
interrupt context, queue locking and wakeup must be IRQ-safe.  Supporting
task-context submission first is consistent with the current LaOS RCU scope,
which does not provide interrupt/NMI read-side variants.

### Barrier, shutdown and backpressure

`rcu_barrier()` waits for callbacks submitted before its barrier record, not
merely for a grace period.  A simple FIFO implementation can enqueue a
completion callback and wait for it.  Shutdown requires this order:

1. stop or reject producers;
2. execute `rcu_barrier()`;
3. stop the callback worker;
4. destroy callback-owned code and state.

Calling `synchronize_rcu()` alone is insufficient because a completed grace
period does not prove that an already-ready callback has run.

Asynchronous updates can accumulate unbounded unreclaimed memory when a
reader stalls.  Required observability includes pending, waiting and executed
callback counts, oldest-callback age, grace-period age and the existing
lagging-CPU/blocked-task state.  The design also needs a bounded test policy
or updater backpressure so callback queuing cannot hide a grace-period stall
until memory exhaustion.

### Validation requirements

The callback implementation is complete only with tests for:

- unlink, submit, pre-existing-reader exit, callback and final free ordering;
- callbacks submitted during a grace period being deferred to the next safe
  batch;
- FIFO ordering for callbacks on the same queue;
- callback resubmission from inside a callback;
- concurrent submitters and worker wakeup without lost work;
- `rcu_barrier()` waiting for callbacks rather than only for a grace period;
- shutdown with an empty queue and with callbacks in every state;
- debug rejection of double submission;
- callback backlog and stalled-reader diagnostics;
- runtime validation on x86_64 and ARM64.

## Configuration-off semantics

With `CONFIG_RCU=0`, `kernel/rcu.h` supplies inline no-op versions of all APIs
and the `rcu_stress` selftest is not registered.

This preserves compilation and removes hot-path overhead.  It does not emulate
a grace period.  Code that deletes and frees concurrently visible objects is
only correct with RCU disabled if another lifetime mechanism replaces it.

## Test structure

`kernel/test_rcu_publish.c` provides the short normal-boot gate.
`kernel/test_rcu_stress.c` provides the configurable focused workload.

These tests have different reclamation coverage.  `rcu_publish` reuses one
static node after `synchronize_rcu()` and therefore checks publication,
observation, unlinking and grace-period ordering, but it does not call
`kfree()`.  `rcu_stress` allocates and frees a node on every round and
deliberately preempts each startup reader, so it exercises the blocked-reader
path and the actual reclaim sequence.

### Configurable stress

- `readers` reader threads, capped at the online CPU count;
- one writer on CPU 0;
- one deliberate context switch in each reader's startup critical section;
- an initial blocked-reader grace period, then one reclamation grace period
  per round;
- a per-round publish/observe handshake before removal;
- timeout, completed-round, visit, blocked-path and empty-list checks.

The canonical result is:

```text
[rcu_stress] PASSED: completed=N/N visits=... blocked_hits=... list_empty=1
[selftest] 'rcu_stress' PASSED
```

It covers forward traversal and a single writer. It does not cover reverse
traversal, concurrent writers or generation wrap. The shorter
`rcu_publish` marker remains required by normal x86_64 and ARM64 Limine gates.
The ARM64 normal gate runs with two CPUs, so the `rcu_publish` reader is
pinned to CPU1 rather than falling back to CPU0.

There is currently no `test-arm64-limine-rcu-stress` target.  ARM64 therefore
has the normal `rcu_publish` runtime gate plus the `LDAR`/`STLR` object-code
check, while the blocked-reader allocation/free stress workload is currently
run through `make test-x86_64-rcu-stress`.

Run the maintained evidence with:

```sh
make test-x86_64
make test-arm64-limine
bash script/check_arm64_rcu_order.sh
```

Expected ARM64 runtime detail is:

```text
[rcu_publish] prepared: rounds=16 reader_cpu=1 timeout=1000
[rcu_publish] PASSED: completed=16/16 list_empty=1
[selftest] 'rcu_publish' PASSED
```

The shell checker is an object-code gate for `LDAR`/`STLR`; the QEMU test is
the bounded cross-CPU progress and data-consistency gate.

The evidence maps to the design as follows:

| Evidence | Exercises | Does not prove |
| --- | --- | --- |
| `rcu_publish` | forward publication, acquire observation, unlink, synchronous reuse | heap free, blocked-reader stress, multiple writers |
| `rcu_stress` | deliberate reader preemption, blocked-list tracking, allocation/free after a grace period | reverse traversal, concurrent writers, sequence wrap |
| `check_arm64_rcu_order.sh` | generated ARM64 acquire/release instruction shape | runtime progress or all compiler/hardware behaviours |
| normal x86_64/ARM64 gates | integration with boot, scheduler and test harness | general RCU API compatibility |

## Failure modes

| Failure | Observable effect |
| --- | --- |
| reader never unlocks | blocked list or CPU generation never advances |
| counted CPU stops ticking/scheduling | stage one waits forever |
| caller holds a reader-needed lock | grace-period deadlock |
| thread exits while linked as blocked | stale blocked-list entry/use-after-free risk |
| premature node free | reader use-after-free |
| release/acquire regression | ARM64 instruction gate fails, or reader observes inconsistent fields |
| sequence wrap with signed comparison | generation ordering becomes ambiguous |

`CONFIG_RCU_DEBUG` helps inspect link/unlink activity, but the implementation
has no stall detector that identifies which condition occurred.

## Evolution order

The safest evolution order is:

1. add invariants and a grace-period stall report;
2. extend release/acquire primitives beyond RCU links when another subsystem needs them;
3. keep the ARM64 instruction and runtime gates required;
4. define and serialize concurrent updater behaviour;
5. add callback records and a `call_rcu()` worker;
6. batch callbacks and grace periods only after correctness tests exist.

Adding asynchronous callbacks before the progress and memory-order contracts
are explicit would hide blocking from callers without removing the underlying
failure modes.
