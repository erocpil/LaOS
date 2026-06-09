# Kernel diagnostics tutorial

Kernel diagnostics have to work when the kernel is already in a damaged state.
That makes them different from ordinary application logging: locks may be
held, stacks may be nearly exhausted, page tables may be wrong, and other CPUs
may still be running.

LaOS provides a useful teaching chain from tagged logs to symbolised exception
reports.  It is deliberately inspectable, but it is not yet a crash-safe,
multi-CPU panic subsystem.

## The diagnostic layers

```text
L() / L_TAG() / kprintf()
          |
          v
framebuffer page + serial log
          |
          v
interrupt/exception entry saves machine state
          |
          v
recoverable user fault? ---- yes ---> map page or terminate task
          |
          no
          v
decode registers/syndrome + symbolise stack
          |
          v
panic / halt current CPU
```

Each layer has a different purpose.  Debug logging explains normal execution;
an exception frame records the point of failure; symbolisation converts raw
addresses into source-level clues; panic stops unsafe continuation.

## Tagged and debug logging

`L_TAG()` in `kernel/log.h` prefixes a message with a subsystem name and uses a
semantic framebuffer colour:

```c
L_TAG(LOG_PMM, "usable pages: %lu\n", pages);
```

`log_init()` computes one common tag width so boot messages align visually.
The fixed tag list covers loader, boot, CPU, PMM, VMM, heap, modules, PCI, TTY,
syscalls and networking.

`L()` is the noisy function-and-line debug macro selected by `CONFIG_DEBUG`.
When disabled, it is placed in an unreachable branch so the compiler still
type-checks the format string and arguments without emitting runtime output.

Use tagged logs for stable subsystem milestones and `L()` for temporary
investigation.  Tests that grep serial output should prefer explicit,
purpose-built markers over incidental debug messages.

## Why serial is the primary crash record

`kprintf()` in `kernel/printf.c` serializes formatting and writes:

- to the active framebuffer TTY when permitted;
- always to the architecture serial console.

The framebuffer may be absent, hidden on another page or damaged by a graphics
bug.  QEMU tests also consume serial logs.  Therefore serial is the closest
thing LaOS has to a canonical diagnostic sink.

It is not yet an emergency sink: `kprintf()` takes `print_lock` and uses a
shared formatting buffer.  If a fault occurs while that lock is held, another
diagnostic call can deadlock.  A production crash path normally needs a
lockless or try-lock polled serial writer.

## Exception entry

The architecture entry assembly must save exactly the layout expected by its C
`struct interrupt_frame`.

On x86_64:

- an IDT stub saves general-purpose registers;
- it normalises vectors with and without a hardware error code;
- the CPU-provided RIP, CS, RFLAGS, RSP and SS complete the frame;
- `idt_handler()` routes page faults separately and other exceptions to the
  fatal reporter.

On ARM64:

- the VBAR_EL1 table has sixteen 128-byte slots;
- common handlers save x0–x30 and the relevant stack/exception state;
- ESR_EL1 is stored as the frame's error code;
- synchronous events use a sentinel interrupt number;
- `idt_handler()` separates SVC, lower-EL aborts and fatal exceptions.

The frame is an ABI between assembly and C.  Field reordering must update
assembly offsets and generated offset checks in the same change.

## Recoverable page faults

Not every exception is a kernel panic.

For a user not-present fault inside a valid VMA, the architecture page-fault
handler allocates and maps a page using the VMA permissions, then returns to
retry the instruction.

If the address is outside a VMA, the access violates permissions, or allocation
fails, the current user thread is marked zombie and the scheduler is invoked.

Kernel faults and faults without a usable current task go to fatal diagnostics.
There is no copy-from-user fixup table, copy-on-write handler or general signal
delivery.

The two architectures decode different hardware state:

| x86_64 | ARM64 |
| --- | --- |
| fault address in CR2 | fault address in FAR_EL1 |
| page-fault error-code bits | ESR exception class and fault status |
| present/write/user/reserved/fetch | translation/access/permission/alignment/abort |

## Fatal x86_64 report

`exception_handler()` in `kernel/arch/x86_64/idt.c` captures CR0, CR2, CR3 and
CR4 early, then reports:

- exception vector and name;
- CPU, current thread name and ID;
- all general-purpose registers;
- RIP, stack and privilege state;
- decoded RFLAGS, CR0 and CR4 feature bits;
- the symbol nearest RIP;
- sixteen instruction bytes at RIP;
- vector-specific error information;
- an x86 four-level page-table walk for a page fault;
- an RBP-based symbolised backtrace.

The page-table walk prints present, writable, user, huge-page, accessed, dirty,
global, cache and NX flags.  This is often enough to distinguish an absent
mapping from a permission or huge-page mistake.

Instruction bytes are a useful final clue, but reading them can itself fault if
RIP is corrupt or unmapped.  The current reporter has no safe probe helper, so
this rich output is best-effort.

## Fatal ARM64 report

`exception_handler()` in `kernel/arch/aarch64/debug.c` reports:

- ESR_EL1 exception class and instruction-specific syndrome;
- FAR_EL1, ELR_EL1, SPSR_EL1 and saved stack;
- all x0–x30 registers;
- decoded data/instruction abort status;
- a frame-pointer chain symbolised through kallsyms.

SVC is normally dispatched before it reaches the fatal reporter.  Lower-EL
translation aborts use the user page-fault path.  same-EL aborts, FIQ, SError
and unhandled synchronous events are fatal.

The frame-pointer walker checks alignment and increasing frame addresses, but
does not validate every address against a known stack mapping before
dereferencing it.  Its output is also best-effort under stack corruption.

## Symbolisation

LaOS uses two symbol tables for different policies:

- `__ksymtab` contains names explicitly exported for module imports;
- `kallsyms_all` contains linked text symbols for diagnostics.

`script/gen_kallsyms.sh` runs `nm -n` after an initial kernel link and emits a C
array.  `kernel.mk` compiles that array and performs a second link.

`kallsyms_lookup()` finds the nearest symbol whose address does not exceed the
reported instruction/return address, then returns the offset.

This lets a trace show:

```text
exception_test_trigger+0x35
exception_test_lv2+0xd
exception_test_lv1+0xd
kmain+0x14c
```

instead of opaque virtual addresses.  The lookup is linear and symbol names do
not include source file/line information.

Both architectures are compiled with frame pointers.  Assembly entry code and
hand-written context transitions can still terminate or confuse a C frame
chain.

## Panic versus an exception

`panic()` is an assertion-style macro used when code detects an invariant it
cannot safely recover from.  It prints function and line context and executes
the architecture halt/wait instruction.

An exception reporter starts from hardware-saved state and prints much more
machine context before halting.

Current panic limitations:

- only the current CPU is stopped;
- other CPUs are not sent a stop IPI;
- there is no single-winner/recursive panic guard;
- printing uses normal locks and shared buffers;
- there is no persistent crash record;
- QEMU is not automatically terminated with a machine-readable reason.

If panic happens in a lock or interrupt path, missing output does not imply the
panic site was never reached.

## Emergency stacks

x86_64 assigns TSS IST stacks to:

- NMI;
- double fault;
- machine check.

This gives those critical exceptions a fresh stack when a thread stack is
unusable.  Ordinary x86 exceptions still use the interrupted stack.

ARM64 IRQ entry switches to a per-CPU interrupt stack, but synchronous
exception handlers use the current exception stack.  There is no separate
fatal synchronous emergency stack comparable to x86 IST.

Emergency stacks reduce recursive failure risk; they do not make `kprintf`,
page-table walking or backtracing safe.

## Fault injection

`CONFIG_EXCEPTION_TEST=1` enables a three-level noinline x86_64 call chain and
triggers a fatal exception late in boot.

`CONFIG_PF_TEST` selects:

- zero: divide error;
- one: page fault at a fixed unmapped address.

The expected report includes full registers, decoded control state, RIP symbol
and bytes, and a symbolised stack.

The IST test uses deliberate kernel-stack exhaustion to distinguish a working
double-fault IST from a triple-fault reboot.

These are destructive tests: success ends with a halted kernel.  They are
disabled by default and do not have dedicated default CI targets.

## A practical debugging workflow

1. Preserve the complete serial log, including the first failure.
2. Identify whether the message came from recoverable page-fault handling,
   fatal exception reporting or `panic`.
3. Record architecture, CPU, current task and privilege level.
4. Decode the syndrome/error code before guessing at subsystem causes.
5. Map RIP/ELR and return addresses to symbols and offsets.
6. Inspect the faulting address and page permissions for memory faults.
7. Check interrupt/preemption state before assuming scheduling could progress.
8. Reproduce with the smallest focused fault injection or QEMU target.
9. Treat a truncated or absent trace as evidence that the diagnostic path may
   also have failed.

## Exercises

1. Add a lockless polled emergency serial formatter with a small stack buffer.
2. Elect one panic CPU and stop the others with architecture IPIs.
3. Validate stack frames against the current thread and per-CPU stack ranges.
4. Add safe instruction-byte and page-table probes that cannot recursively
   fault.
5. Emit a compact machine-readable panic trailer for test harnesses.
6. Add source file/line symbol information or host-side address decoding.
