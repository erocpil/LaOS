# RCU tutorial

Read-Copy-Update (RCU) is useful when a data structure is read very frequently
and changed rarely.  It moves most synchronisation cost away from readers:
readers enter a lightweight critical section, while a writer waits before
reclaiming an object that readers may still reference.

LaOS implements a small preemptible RCU model for teaching.  It demonstrates
reader lifetime, quiescent states, grace periods and deferred reclamation, but
it is not a Linux-compatible RCU implementation.

## The problem RCU solves

Consider a shared linked list.  A mutex makes traversal safe, but every reader
must acquire the same lock:

```text
reader: lock -> traverse -> unlock
writer: lock -> remove -> free -> unlock
```

RCU changes the ownership rule:

```text
reader: rcu_read_lock -> traverse -> rcu_read_unlock

writer: remove from published structure
        -> synchronize_rcu
        -> free
```

After removal, new readers can no longer find the object.  Existing readers
may still hold it, so the writer waits for a grace period before freeing it.

## Read-side critical sections

The reader API is declared in `kernel/rcu.h`:

```c
rcu_read_lock();
/* dereference RCU-protected objects */
rcu_read_unlock();
```

LaOS stores a nesting count in the current thread.  Entering increments it and
leaving decrements it.  Nested critical sections are allowed; only the outer
unlock marks the thread as no longer reading.

The critical section is preemptible.  `rcu_read_lock()` does not disable
interrupts or scheduler preemption.  This is the central teaching point: a
reader may be switched out while it still holds an RCU reference.

Reader rules:

- do not sleep because an API name says RCU; sleep only if the protected
  object's own lifetime and operation permit it;
- do not carry an RCU-protected pointer past the outer unlock;
- do not free or repurpose an object visible to another reader;
- keep every lock/unlock pair balanced, including error paths;
- do not use this implementation's read-side API from an IRQ top half.

RCU protects object lifetime.  It does not automatically make mutable fields
inside an object race-free.

## Quiescent states

A quiescent state is evidence that a CPU is no longer executing an older
read-side critical section.

LaOS records quiescent states in two places:

1. `rcu_check_quiescent_state()` runs from the timer tick.  If the current
   thread has nesting zero, the CPU records the latest grace-period sequence.
2. `rcu_note_context_switch()` runs before the scheduler switches away from
   the current thread.  A non-reader switch is a quiescent state.

Every CPU context contains the last grace-period sequence it has observed.
This gives an updater a small per-CPU progress table.

## Preempted readers

A CPU reaching a quiescent state is not enough for preemptible RCU.  A thread
may have entered a read-side section and then been switched out; its CPU can
subsequently run other non-reader threads and report quiescent states.

Before switching such a reader out, LaOS places the thread on a global
`blocked_tasks` list:

```text
reader enters
    -> timer preempts reader
    -> scheduler records reader in blocked_tasks
    -> other work runs on that CPU
    -> reader is scheduled again
    -> outer rcu_read_unlock removes blocked record
```

The blocked-list node and flag live in the thread control block.  A spinlock
protects the global list.

## The two-stage grace period

`synchronize_rcu()` in `kernel/rcu.c` is a blocking updater operation:

```text
increment global generation
        |
        v
wait until every online CPU has observed this generation
        |
        v
wait until blocked_tasks is empty
        |
        v
return: pre-existing readers have finished
```

The first stage catches readers that continue running without a context
switch.  The second catches readers that were preempted and no longer appear
as the current reader on any CPU.

Waiting calls `schedule()` rather than spinning continuously.  The function
must therefore be called from schedulable thread context with no spinlock held.
It is not an interrupt-context API.

## Asynchronous reclamation and `call_rcu()`

LaOS does not currently implement `call_rcu()`.  This section records the
intended API contract so that adding asynchronous reclamation does not change
the lifetime rules demonstrated by the synchronous implementation.

`call_rcu()` is the callback form of `synchronize_rcu()`.  It does not provide
a different grace-period algorithm or make a grace period shorter.  Instead,
it queues reclamation work and returns immediately; RCU invokes the callback
after a grace period that covers every read-side critical section that could
already hold the removed object.

A typical object embeds the queue record:

```c
struct item {
	int value;
	struct list_node node;
	struct rcu_head rcu;
};

static void free_item(struct rcu_head *head)
{
	struct item *item = container_of(head, struct item, rcu);
	kfree(item);
}
```

The updater must first make the object unreachable and only then submit its
callback:

```c
spin_lock(&update_lock);
list_del_rcu(&item->node);
spin_unlock(&update_lock);

call_rcu(&item->rcu, free_item);
```

The safety argument is:

```text
old reader obtains item
        |
updater unlinks item
        |
updater queues callback and continues
        |
        | grace period covers readers that might hold item
        v
callback frees item
```

- A reader that obtained `item` before the unlink might still dereference it,
  so the callback must wait for that reader's outermost unlock.
- A reader that starts after the unlink cannot discover `item` through the
  protected structure.
- New, unrelated readers may run concurrently with the callback.  RCU waits
  for readers that might hold the old pointer, not for a global
  "no readers anywhere" state.

This is why `call_rcu()` must follow the unlink.  Queueing the callback first
would leave a window in which another reader could acquire the object without
necessarily being covered by the callback's grace period.

Conceptually, `struct rcu_head` contains the callback-queue link and callback
function.  Embedding it avoids a second allocation and lets the callback
recover the enclosing object with `container_of()`.  Once submitted:

- the enclosing object and its `rcu_head` must remain allocated until the
  callback completes;
- the same `rcu_head` must not be queued twice concurrently;
- code must not repurpose the object while callback ownership is outstanding;
- a callback may resubmit its `rcu_head` only as a new operation after the
  current invocation has taken ownership of it.

The three related waiting interfaces have different completion conditions:

| Interface | Caller waits for | Completion meaning |
| --- | --- | --- |
| `synchronize_rcu()` | one grace period | pre-existing readers have completed |
| `call_rcu()` | queue insertion only | callback will run after its grace period |
| `rcu_barrier()` | queued callbacks | callbacks submitted before the barrier have completed |

In particular, `synchronize_rcu()` is not a callback-queue drain.  Shutdown or
module unload must first prevent new callback submissions, then use an
`rcu_barrier()`-style operation before destroying callback code or state.

Asynchronous reclamation also moves, rather than removes, backpressure.  A
slow or stuck reader blocks the grace period while updaters continue queueing
objects.  A practical implementation therefore needs callback-backlog
metrics, a submission-rate policy and stall diagnostics.

The planned LaOS queue, batching and worker rules are described in the
[architecture companion](rcu-arch.md#planned-asynchronous-callback-architecture).
The Linux API contract is documented in
[What is RCU?](https://docs.kernel.org/RCU/whatisRCU.html) and the
[`rcu_barrier()` documentation](https://docs.kernel.org/RCU/rcubarrier.html).

## Using the RCU list helpers

`kernel/list.h` provides:

- `list_add_rcu()` and `list_add_tail_rcu()`;
- `list_del_rcu()`;
- `list_for_each_rcu()` and `list_for_each_entry_rcu()`.

A minimal pattern is:

```c
/* Writers must still serialize with other writers. */
spin_lock(&update_lock);
new_item->value = value;
list_add_rcu(&new_item->node, &items);
spin_unlock(&update_lock);

rcu_read_lock();
list_for_each_entry_rcu(item, &items, node) {
    consume(item->value);
}
rcu_read_unlock();

spin_lock(&update_lock);
list_del_rcu(&old_item->node);
spin_unlock(&update_lock);
synchronize_rcu();
kfree(old_item);
```

`list_del_rcu()` intentionally leaves the removed node's links intact until
the grace period finishes, because a reader already standing on that node may
need its `next` link.

Writer mutual exclusion is separate from reader protection.  RCU does not
allow two writers to update ordinary list links without a writer lock or an
equivalent single-writer rule.

## Memory ordering boundary

Publication has two requirements:

1. initialise an object before publishing the link that makes it reachable;
2. make a reader that observes the link also observe the initialised fields.

The RCU list helpers use a release-store for forward-link publication and an
acquire-load for traversal.  Compiler atomics preserve the same source-level
contract on all supported architectures: x86_64 normally needs no extra
instruction, while ARM64 lowers the operations to `STLR` and `LDAR` (or an
equivalent sequence).

Therefore:

- the RCU grace-period core is compiled into shared code;
- the bounded `rcu_publish` selftest validates publish, observe, unlink and
  grace-period reuse on both x86_64 and ARM64;
- the ARM64 build gate also inspects the generated acquire/release
  instructions, because a QEMU run alone is weak evidence for memory ordering.

This distinction matters: waiting for old readers and publishing new data are
related but separate correctness problems.

## Configuration and experiments

`CONFIG_RCU` in `kernel/config.h` enables the implementation.  When disabled,
the reader, updater and integration hooks compile to no-op stubs.  This is
useful for isolating scheduler problems, but disabling RCU also removes its
lifetime guarantee; callers must not reclaim shared objects as if a grace
period occurred.

`CONFIG_RCU_DEBUG` prints blocked-list transitions and is intentionally noisy.

The built-in `rcu_stress` selftest accepts `rounds`, `readers` and
`timeout_ticks`. Each reader deliberately calls `schedule_timeout(1)` while
inside an RCU critical section. This deterministically exercises the
preemptible-reader `blocked_tasks` path instead of relying on a timer interrupt
landing in a busy loop.

The writer performs a plain grace period and then a publish/observe/remove/
grace-period/free cycle for every round. Run the focused x86_64 gate with:

```sh
make test-x86_64-rcu-stress
```

The stress test demonstrates the intended multi-CPU paths. It does not prove
all compiler transformations, weak-memory orderings, multiple updater
behaviour, CPU hotplug or arbitrary hardware stalls.

## What LaOS RCU does not provide

- `call_rcu()` or an asynchronous callback worker;
- batching of reclamation callbacks;
- expedited grace periods;
- stall detection and diagnostics;
- CPU hotplug integration;
- interrupt/NMI read-side variants;
- a general release/acquire API outside the RCU list helpers;
- a stable externally consumable RCU ABI.

The current updater waits synchronously, so a slow or stuck reader directly
delays the writer.

## Suggested exercises

1. Add assertions for unlock underflow and thread exit with nonzero nesting.
2. Add a bounded stall diagnostic that prints lagging CPUs and blocked tasks.
3. Introduce architecture-correct `smp_store_release` and
   `smp_load_acquire`, then run the list test on ARM64.
4. Serialize or explicitly design concurrent grace-period updaters.
5. Add `call_rcu()` with a callback queue and worker, preserving callback
   order and shutdown semantics.
