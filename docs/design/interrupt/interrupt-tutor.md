# Interrupt tutorial

[中文版](interrupt-tutor-zh.md)

An interrupt suspends an instruction stream. Before C code can run, the entry
code must preserve enough state to resume that stream correctly. Start with
the [boot tutorial](../boot/boot-tutor.md) if IDT and per-CPU setup are unfamiliar.

## 1. Follow a timer tick

Read the timer IRQ stub in `kernel/arch/x86_64/idt_stubs.S`, then follow
`common_stub`, `idt_handler()` and `irq_handler()`. Timer vector 32 reaches
`timer_handler()` in `kernel/timer.c`.

```text
timer -> IDT stub -> saved interrupt frame -> C handler
      -> pending reschedule check -> restore -> iretq
```

The tick can request a switch. The request and the actual switch are separate
events; the common return path decides whether scheduling is currently allowed.

## 2. Match assembly to C

Compare `PUSH_ALL` with `struct interrupt_frame` in
`kernel/arch/x86_64/idt.h`. Push order is the reverse of the final memory order.
Then compare an error-code exception stub with a no-error-code stub: both
must present the same vector/error-code pair to C.

Find the saved CS test and explain when GS must change. A kernel interrupt
during a syscall already has kernel GS state, even though a user program
originally caused the kernel to run.

## 3. Keep the continuation on its owner's stack

If a timer preempts a syscall and switches to another thread, the first
thread still needs its syscall frame, C call frames and interrupt frame.
Those frames remain on its kernel stack. On resumption, the interrupt returns
to the syscall, and the syscall later returns to user mode.

Dedicated fatal-exception stacks serve another purpose: allowing diagnosis
when the normal stack cannot be trusted. They do not imply that every IRQ
uses a separate per-CPU stack.

## 4. Distinguish a fault from an external IRQ

A page fault describes a problem with an instruction's memory access. LaOS
may repair a missing user mapping and retry the instruction. A device IRQ
describes device work and may require clearing a device-specific cause.
Only APIC-delivered interrupts require the corresponding controller EOI.

Follow both paths in `idt_handler()` before adding an acknowledgement to a
shared return path. An unconditional EOI would blur this distinction.

## 5. Observe cross-CPU delivery

```sh
make test-x86_64
make test-x86_64-smp-tlb
```

Use the named `ipi_delivery`, `remote_enqueue` and `smp_tlb_remap` results to
separate three claims: an interrupt arrived, remote work ran, and a changed
translation became visible. None of these claims follows from the other
two without checking its own test.

For a deliberate exception experiment, follow the configuration and expected
halt behavior in the [diagnostics tutorial](../diagnostics/diagnostics-tutor.md).

## 6. Exercise: reconstruct a nested return

Write the return sequence for user code entering `write`, a timer interrupting
the handler, and another thread running before the original resumes. Mark
which return uses `iretq` and which uses `sysret`, and identify the owner of
each frame. Compare your answer with [Chapter 3](../../book/03-interrupt.md).

## 7. Exercise: separate acknowledgement from progress

Choose the e1000 path and explain why an IRQ count can increase while RX
processing remains stalled. Identify which source clears `ICR`, which path
performs controller acknowledgement, and which worker eventually consumes the
descriptor.
