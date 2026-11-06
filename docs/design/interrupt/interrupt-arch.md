# Interrupt architecture

[中文版](interrupt-arch-zh.md)

## Scope

The x86_64 entry code turns hardware exceptions, device IRQs and IPIs into
structured C calls. The return path also provides a scheduler preemption
point. System calls have a separate entry and frame, described in the
[user architecture](../user/user-arch.md).

## Source map

| File | Responsibility |
| --- | --- |
| `kernel/arch/x86_64/idt_stubs.S` | register save/restore, vector stubs and common return |
| `kernel/arch/x86_64/idt.h` | interrupt frame and IDT layout |
| `kernel/arch/x86_64/idt.c` | gates, exception/IRQ/IPI dispatch |
| `kernel/arch/x86_64/lapic.c` | local APIC and I/O APIC setup |
| `kernel/arch/x86_64/ipi.c` | cross-CPU requests and test observers |
| `kernel/arch/x86_64/ist.c` | dedicated exception stacks |
| `kernel/timer.c` | timer work and reschedule requests |

## Entry and frame contract

Some exceptions push an error code; others do not. The stubs supply a zero
where necessary and add a vector number. `PUSH_ALL` then saves the general
registers so the stack matches `struct interrupt_frame`:

```text
low address: rax ... r15 | int_no | error_code | rip | cs | rflags | rsp | ss
```

Assembly and C must agree on this ordering. The common path uses saved CS to
distinguish entry from user mode and applies the corresponding GS transition.
It restores the saved context and returns with `iretq`.

The syscall `trap_frame` has a different layout and no vector/error-code
pair. A pointer to one frame cannot be passed to a handler expecting the other.

## Stack ownership

An interrupt from user mode selects a kernel stack through TSS state. An
ordinary IRQ interrupting kernel execution stays on that thread's kernel
stack. Fatal exception gates can select dedicated IST stacks.

Per-CPU IRQ stacks are allocated, but the ordinary IRQ path does not switch
frames onto them. This matters during preemption: a suspended IRQ continuation
must remain attached to the interrupted thread until it resumes. Reusing one
CPU-wide stack for several suspended threads would overwrite their frames.

The [exception note](../diagnostics/exception-handler.md) covers detailed
diagnostic output and IST behavior.

## Dispatch and acknowledgement

`idt_handler()` handles IPI vectors before the ordinary exception/IRQ split:

| Source | Work |
| --- | --- |
| TLB IPI | EOI, local TLB flush and registered observers |
| Selftest IPI | EOI and selftest callbacks |
| Reschedule IPI | EOI and acknowledgement; common return checks pending work |
| Page fault | attempt user demand paging, otherwise diagnose or terminate |
| Timer vector 32 | timer handler and CPU tick accounting |
| Keyboard vector 33 | input handling |
| Cached e1000 vector | invoke the loaded driver's callback |

CPU exceptions do not receive an APIC EOI. For device interrupts, clearing
the device cause and acknowledging the APIC are different operations. The
e1000 callback reads its cause register; the common IRQ path handles APIC
acknowledgement.

The acknowledgement contract is therefore:

```text
enter vector -> save frame -> identify source
             -> service/clear device or IPI cause
             -> acknowledge the interrupt controller when required
             -> request reschedule if pending
             -> restore the interrupted frame
```

An EOI without clearing a level-triggered device cause can retrigger the
interrupt. Clearing a device cause without the controller EOI can leave the
controller state pending. These are separate failure classes and should be
diagnosed separately.

## Preemption and remote work

The common return path checks `need_resched` before restoring the frame.
Scheduler entry is valid only when the current context permits preemption;
spinlock and preemption state are part of that decision.

Remote enqueue publishes runqueue state and requests rescheduling. A local
request sets the same pending flag without an IPI to itself. The x86_64
remote path currently broadcasts because logical CPU indices do not provide
a general reverse mapping to LAPIC IDs.

The return path has a strict ordering boundary: the handler may set
`need_resched`, but scheduling must not overwrite the frame before the current
interrupt continuation has been saved. A context switch changes the thread
that owns the continuation; it does not copy an interrupt frame to a shared
CPU stack.

## Architecture and ownership boundary

| Concern | x86_64 | ARM64 |
| --- | --- | --- |
| saved frame | `struct interrupt_frame`, `iretq` return | exception frame owned by the EL/architecture entry path |
| device acknowledgement | APIC EOI after device-cause handling | GIC acknowledgement/EOI path after source handling |
| reschedule notification | LAPIC IPI or local flag | directed SGI or local flag |
| ordinary IRQ stack | interrupted thread's kernel stack | interrupted exception context and architecture entry stack rules |

The shared scheduler and device callbacks do not remove these entry-path
differences. Any change to frame layout, vector routing or EOI order needs a
per-architecture validation gate.

## Validation and limits

```sh
make test-x86_64
make test-x86_64-smp-tlb
make test-x86_64-sched-stress
```

These cover ordinary startup, named IPI tests, remote enqueue and scheduling
stress. They do not validate arbitrary interrupt affinity, CPU hotplug or a
general driver registration framework. Deliberate fatal-exception tests halt
the kernel and must be interpreted separately from normal boot gates.

Evidence should distinguish four claims: frame restoration, controller
acknowledgement, device-cause handling and reschedule delivery. A passing IPI
test does not validate an e1000 cause path, and an increasing e1000 IRQ count
does not validate frame restoration or packet progress.
