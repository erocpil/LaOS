# Chapter 3: Interrupts

[中文版](03-interrupt-zh.md)

> **Prerequisites**: [Boot](01-boot.md), [Memory](02-memory.md) and stack layout.
> **You'll build**: a model of saved frames, nested entry and safe return.
> **Cross-reference**: [interrupt tutorial](../design/interrupt/interrupt-tutor.md)
> and [interrupt architecture](../design/interrupt/interrupt-arch.md).

---

## 3.1 Preserve the interrupted instruction stream

An interrupt can arrive between instructions in code that did not call the
handler. Ordinary C calling conventions are therefore insufficient: the
entry must preserve state the interrupted code expects to keep.

In `kernel/arch/x86_64/idt_stubs.S`, hardware state, a normalized error-code
slot, the vector number and general registers form `struct interrupt_frame`.
The C handler receives a pointer to that saved state.

Exceptions with hardware error codes and those without them must reach C
with the same layout. Otherwise every field after the missing word would
describe the wrong value.

## 3.2 Determine where the frame lives

An interrupt from ring 3 uses kernel stack state established through the
TSS. A normal interrupt during kernel execution stays on the current thread's
stack. Selected fatal exceptions use IST stacks to improve diagnostic
survivability.

The allocated per-CPU IRQ stacks are not used for ordinary frame switching
on this x86_64 path. A stack's existence is not evidence of its use.

## 3.3 Scheduling can suspend the handler

Consider a timer interrupt arriving during a write syscall:

```text
user write
  syscall frame
    syscall C calls
      interrupt frame -> timer handling -> scheduler switch
                                          another thread runs
                                      <- original thread resumes
      iretq -> syscall C resumes
  sysret -> user resumes
```

Both saved frames remain on the original thread's kernel stack. Sharing that
storage with another suspended continuation would make resumption unsafe.
The scheduler's own saved switch frame adds another layer rather than
replacing the syscall or interrupt frame.

## 3.4 Dispatch determines recovery

A not-present user page fault can be repaired through the VMA path. An
invalid user access may terminate that thread. A kernel exception enters
diagnostics. Those outcomes are selected by fault classification and current
context, not merely by the presence of an IDT gate.

An external device IRQ has a different obligation: service or record the
device cause and acknowledge the interrupt controller. A CPU exception does
not require an APIC EOI. For the detailed failure display, read the
[diagnostics tutorial](../design/diagnostics/diagnostics-tutor.md).

## 3.5 An IPI conveys a request

The kernel uses IPIs for TLB invalidation, delivery selftests and remote
rescheduling. Queue updates and `need_resched` publication must precede the
reschedule notification. The receiver then acts on published state.

For TLB work, delivery and completion of the required invalidation are
distinct observations. This distinction connects interrupts back to the
memory-reclamation problem in Chapter 2.

## Experiment: reconstruct a return path

```sh
make test-x86_64-smp-tlb
```

Follow the test's IPI and remap evidence. Then trace the timer return in
assembly and identify where scheduling can happen and where registers are
restored. Compare that path with syscall return; explain why one frame cannot
be passed to the other's restore code.

**Previous**: [Memory](02-memory.md). **Next**: [Scheduling](04-scheduler.md).
