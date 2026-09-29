# User-mode architecture

[中文版](user-arch-zh.md)

## Scope

LaOS runs small, statically linked user ELFs and provides six teaching
syscalls. The current x86_64 path supports private user roots, kernel-stack
continuations, preemptible syscall handling and lazy mappings. It does not
implement `fork`, `execve`, `waitpid`, signals or a POSIX descriptor table.

## Source map

| File | Responsibility |
| --- | --- |
| `kernel/elf_loader.c` | configured user-thread construction |
| `kernel/elf.c` | ELF segment copying and initial user stack |
| `kernel/thread.c` | thread lifecycle and destruction |
| `kernel/arch/x86_64/thread_arch.h` | initial switch-frame construction |
| `kernel/arch/x86_64/syscall_entry.asm` | syscall entry, stack switch and return |
| `kernel/arch/x86_64/syscall.h` | syscall numbers and trap-frame layout |
| `kernel/arch/x86_64/syscall.c` | MSRs, dispatch and first user transition |
| `user/crt0.s`, `user/user.lib.c` | ELF entry and syscall wrappers |

## Construction and first entry

Configured user tasks call `create_elf_process()`. It copies the ELF image,
initializes the TCB and priority state, creates a user page-table root, loads
`PT_LOAD` segments, prepares the argument stack and allocates a private kernel
stack. The task layer subsequently assigns and enqueues the thread.

The root shares kernel high-half mappings. ELF data is copied through HHDM
aliases of physical pages. Segment permissions derive from ELF flags; BSS
bytes within the loaded segment are cleared. The user stack contains `argc`,
user-space `argv` pointers and strings. It is not a full hosted process
startup environment with a dynamic linker and auxiliary vector.

The architecture entry stub prepares `RIP`, `CS`, `RFLAGS`, `RSP` and `SS` and
uses `iretq` for first entry to ring 3. `user/crt0.s` converts the stack layout
to C arguments and calls `main`. User code should call `exit()` explicitly:
the current CRT fallback executes `hlt` if `main` returns.

There is also a local embedded-ELF demonstration in
`kernel/arch/x86_64/main.c`. Its `load_user_elf()` path uses the kernel root.
It must not be described as another instance of the private-root loader.

## Syscall ABI

`RAX` contains the call number. Arguments use `RDI`, `RSI`, `RDX`, `R10`,
`R8`, `R9`; the result is returned in `RAX`. Current calls use at most three
arguments. `RCX` and `R11` are hardware-clobbered by `syscall`.

| Number | Interface | Current result convention |
| --- | --- | --- |
| 1 | `write(fd, buf, count)` | requested count or -1; only fd 1 and 2 |
| 3 | `yield()` | 0 after scheduling |
| 5 | `mmap(length, prot, flags)` | virtual address or 0 |
| 6 | `munmap(addr, length)` | 0 or -1; exact VMA match |
| 35 | `msleep(msec)` | result of tick-based sleep; zero requests a yield |
| 60 | `exit(status)` | does not return |

Unknown calls return -1. These numbers and semantics are a LaOS ABI, not a
Linux syscall compatibility layer. See the
[memory architecture](../memory/memory-arch.md) for mapping restrictions.

## Complete syscall frame

The assembly entry changes GS, saves the user RSP in a per-CPU scratch slot,
and switches to the current thread's kernel-stack top. It builds an 18-word,
144-byte `struct trap_frame` with this order from low to high address:

| Offset | Fields, in memory order |
| --- | --- |
| 0–32 | `rax`, `r12`, `r13`, `r14`, `r15` |
| 40–80 | `r9`, `r8`, `r10`, `rdx`, `rsi`, `rdi` |
| 88–96 | `rbx`, `rbp` |
| 104–136 | `rip`, `cs`, `rflags`, `rsp`, `ss` |

`rip` contains the return address supplied in RCX; `rflags` contains the
flags supplied in R11. There are no separate saved values for the user RCX
and R11 from before the instruction, because the hardware has already
overwritten them. All other general registers needed by the syscall return
path have slots. FPU state belongs to the thread's separate context-switch
save area, not to this frame.

`syscall_handler()` writes the result into `regs->rax`. Assembly restores
the registers, loads RCX/R11 from the return fields, restores user RSP,
changes GS back and executes `sysret`. The saved CS/SS slots describe the
context, but this return path obtains selectors through the MSR contract;
it does not pop them into segment registers.

Frame ordering is handwritten in assembly and C. Generated offsets cover
per-CPU/thread accesses and selector constants, not every trap-frame field.
Any layout change must update both definitions and verify stack alignment
at the C call boundary.

## Preemption and frame lifetime

Entry masks interrupts through SFMASK. C enables interrupts after the frame
is complete. A timer can therefore suspend the syscall on its thread's
kernel stack. Per-CPU user-RSP storage is only scratch during the protected
entry/exit intervals; the durable value is the frame's RSP slot.

On exit, C disables interrupts and drains pending reschedule requests using
`schedule()`. It then returns to the assembly restore path. The syscall frame
is not an IRQ frame, so returning through the IRQ scheduling/restore contract
would be incorrect.

## Process-model and validation limits

A complete saved frame is one prerequisite for cloning execution state.
`ret_from_fork` in switch assembly is a first-run thread trampoline; its name
does not mean a fork syscall exists. Fork still needs address-space cloning,
child/parent result setup, ownership and failure cleanup, and process-lifecycle
semantics.

Other current limits include incomplete malformed-ELF bounds checking, no
general fault-recovering user-copy API, and no comprehensive validation of a
synthetic `sysret` target. `write` performs range/count checks but directly
reads user memory and prints string chunks; it is not a binary-safe POSIX
write implementation or a safe probe of arbitrary user pointers.

```sh
make test-x86_64
make test-x86_64-multiuser
```

The second target counts at least three user completion messages. It provides
useful lifecycle smoke evidence, but not a dedicated register-sentinel test
for every saved register, proof of successful exit reclamation, or a fork
test. The [user tutorial](user-tutor.md) explains how to inspect the path.
