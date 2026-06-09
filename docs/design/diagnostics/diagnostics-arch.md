# Diagnostics and exception architecture

## Scope

The diagnostics subsystem combines:

- formatted/tagged logging;
- serial and framebuffer sinks;
- architecture exception entry and dispatch;
- recoverable user page faults;
- fatal register/syndrome reporting;
- kallsyms-based stack traces;
- architecture halt primitives.

The code is useful for teaching and QEMU diagnosis.  It does not guarantee a
complete crash record under lock corruption, recursive faults, stack failure
or multi-CPU activity.

## Component ownership

| Component | Responsibility |
| --- | --- |
| `kernel/log.c`, `kernel/log.h` | tag metadata and alignment |
| `kernel/printf.c` | formatting, print serialization and output sinks |
| `kernel/ksym.c` | module symbol lookup and diagnostic address lookup |
| `script/gen_kallsyms.sh` | generate full linked-function table |
| `kernel/debug.h` | select architecture diagnostic interface |
| `kernel/arch/x86_64/idt_stubs.S` | x86 register-frame construction |
| `kernel/arch/x86_64/idt.c` | x86 dispatch and fatal report |
| `kernel/arch/x86_64/page_fault.c` | x86 recoverable user faults |
| `kernel/arch/x86_64/debug.c` | x86 general stack dump and halt |
| `kernel/arch/aarch64/entry.S` | ARM64 vector table and frame construction |
| `kernel/arch/aarch64/idt.c` | ARM64 IRQ/synchronous dispatch |
| `kernel/arch/aarch64/page_fault.c` | ARM64 recoverable user aborts |
| `kernel/arch/aarch64/debug.c` | ARM64 fatal report and stack dump |

Architecture paths exist together only on the ARM64 branch; the `x86_64`
shared-source branch intentionally lacks `kernel/arch/aarch64/`.

## Logging contracts

### Formatting

`kprintf()` and `kprintf_color()` format into one global buffer under
`print_lock`.  Truncated messages receive a visible suffix.  `%n` is consumed
but disabled.

The formatting lock is taken with local interrupts disabled.  Callers must not
re-enter printing while already holding the lock.

### Sinks

Serial is always attempted.  Framebuffer output is conditional on boot/TTY
visibility.  Coloured output uses colour only on the framebuffer; serial
contains plain text suitable for grep.

This makes serial markers an intentional test interface.  Renaming a gated
message requires changing its harness atomically.

### Tagged logging

`L_TAG` indexes a compile-time table by `log_module_e`.  The static assertion
keeps enum and table size aligned, but callers still must pass a valid enum
value.

`L` compiles runtime printing in or out through `CONFIG_DEBUG`.  It should not
be used as a required test oracle.

## x86_64 frame and dispatch

`struct interrupt_frame` in `kernel/arch/x86_64/idt.h` must match the push order
in `kernel/arch/x86_64/idt_stubs.S`.

The stubs normalise error-code layout so C always sees:

```text
GPRs
vector
error code
RIP, CS, RFLAGS, RSP, SS
```

`idt_handler()` routes:

- hardware vectors to `irq_handler()`;
- page fault vector 14 to `page_fault_handler()`;
- other CPU exceptions to `exception_handler()`;
- selected IPI vectors to their handlers.

Fatal-report ordering captures CR2 before printing because a later nested page
fault could overwrite it.

NMI, double fault and machine check use dedicated TSS IST slots.  Other
exceptions and ordinary IRQs retain different stack assumptions; x86 ordinary
IRQs still consume the interrupted thread's kernel stack.

## ARM64 frame and dispatch

`SAVE_ALL` in `kernel/arch/aarch64/entry.S` allocates a 296-byte frame, stores
x0–x30, saved SP, ELR_EL1, SPSR_EL1, event identifier and ESR_EL1.  SP_EL0 is
saved for lower-EL events.

Vector routing:

- EL1t and EL1h synchronous events enter the fatal exception path;
- EL0t synchronous events go through `idt_handler()`;
- SVC reaches the syscall handler;
- lower-EL instruction/data aborts reach the page-fault handler;
- FIQ and SError reach fatal reporting;
- IRQ entry switches to the per-CPU interrupt stack.

Unimplemented vector slots emit a unique UART character and loop.  This is an
early-emergency breadcrumb, not a full report.

ARM64 fatal reporting decodes ESR exception classes and instruction/data abort
status.  It then invokes `panic()`, producing a final panic line before waiting.

## Page-fault decision table

| Condition | Result |
| --- | --- |
| user/lower-EL translation fault, address in VMA | allocate/map page and retry |
| user fault, no VMA | mark thread zombie and schedule |
| user permission fault | log access type, kill thread |
| allocation/mapping failure | kill thread |
| x86 kernel page fault | fatal x86 exception report |
| ARM64 same-EL abort | fatal ARM64 exception report |
| no current task | fatal path |

The demand mapper checks whether another path has already installed the page
before allocating.  It is not a complete per-address fault serialization
scheme for multiple threads sharing an address space.

The task-kill path enables interrupts and schedules away.  Returning from that
schedule call is treated as a panic invariant violation.

## Symbol tables

`EXPORT_SYMBOL` entries define the module import namespace.
`kallsyms_all` defines the diagnostic text namespace.

The kernel build is two-pass:

```text
link provisional kernel
  -> nm -n text symbols
  -> generate kallsyms_all.c
  -> compile generated object
  -> final link
```

`kallsyms_lookup()` searches both tables for the greatest address not exceeding
the requested address.  Complexity is linear in the number of symbols.

Limits:

- no source file or line numbers;
- no compressed names;
- no module-runtime symbol registration in diagnostic kallsyms;
- a corrupt address can be attributed to the nearest earlier symbol even when
  it lies outside that function's true size;
- return addresses conventionally point after the call instruction.

## Stack walking

Both kernel builds use `-fno-omit-frame-pointer`.

x86_64 walkers follow:

```text
RBP[0] -> previous RBP
RBP[1] -> return address
```

The fatal exception walker limits depth to eight and accepts canonical
high-half addresses.  The generic `dump_stack()` allows up to 32 frames.

ARM64 follows:

```text
FP[0] -> previous x29
FP[1] -> saved x30/LR
```

It requires alignment and monotonically increasing frame addresses, with a
depth limit of 32.

Neither walker fully validates that every frame lies inside a known live stack
range before dereferencing it.  Hand-written assembly, tail calls, corrupted
frames and transitions between thread/interrupt stacks can truncate or
misdirect a trace.

## Panic state machine

The effective current state machine is:

```text
detect invariant failure
  -> normal kprintf panic line
  -> disable/halt or wait on current CPU forever
```

Missing states:

- atomic election of a primary panic CPU;
- recursive-panic fallback;
- stop acknowledgement from other CPUs;
- lockless emergency output;
- per-CPU register capture;
- reboot/poweroff/debugger policy;
- durable dump completion marker.

Because other CPUs continue, shared state and output can still change after
one CPU panics.  The x86 macro's description as a whole-system halt should not
be treated as an implemented guarantee.

## Diagnostic-path hazards

| Hazard | Current consequence |
| --- | --- |
| panic while holding `print_lock` | recursive print deadlock |
| exception while framebuffer/serial code is broken | partial or absent output |
| invalid RIP instruction-byte read | nested page fault |
| corrupt page-table pointer during walk | nested page fault |
| corrupt RBP/FP chain | truncated trace or nested fault |
| overflowed current kernel stack | ordinary exception may fail before report |
| concurrent panics | interleaved output and independent halted CPUs |
| early fault before CPU context | some paths guard; guarantees are not uniform |

Rich diagnostics increase the amount of code executed after a fault.  A future
design should have a minimal emergency tier and an optional rich tier.

## Test and fault-injection matrix

| Mechanism | Configuration | Evidence |
| --- | --- | --- |
| x86 divide exception | `CONFIG_EXCEPTION_TEST=1`, `CONFIG_PF_TEST=0` | vector decode, registers, RIP bytes and symbolised call chain |
| x86 kernel page fault | `CONFIG_EXCEPTION_TEST=1`, `CONFIG_PF_TEST=1` | CR2/error decode and page-table walk |
| x86 double-fault IST | IST test configuration | dedicated-stack marker rather than triple-fault reboot |
| user demand paging | normal/multi-user tests | valid VMA faults map and tasks continue |
| invalid user access | page-fault test paths | task termination without kernel halt |
| ARM64 syndrome report | manual fault/debug paths | ESR/FAR decode and FP trace |
| panic absence | several QEMU scripts | no `[PANIC ` marker before expected success |

The destructive fatal tests are disabled by default.  There is no maintained
Make target that enables them, verifies a complete field set and restores the
configuration automatically.  The normal smoke tests do not prove emergency
diagnostics.

## Evolution order

1. Add a raw polled serial writer that does not use `print_lock`, heap or the
   shared format buffer.
2. Add atomic primary-panic election and cross-CPU stop IPIs.
3. Add recursive-fault/panic detection that falls back to one-line raw output.
4. Validate instruction, page-table and stack addresses before dereference.
5. Track all thread/per-CPU stack ranges for architecture walkers.
6. Add deterministic fatal-test Make targets and machine-readable completion
   markers.
7. Add optional source-line decoding or export an address list for host tools.
