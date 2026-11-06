# Chapter 5: User Mode

[中文版](05-user-mode-zh.md)

> **Prerequisites**: Chapters [2](02-memory.md)–[4](04-scheduler.md): mappings,
> saved frames and scheduling.
> **You'll build**: a trace from a configured ELF to a returning syscall.
> **Cross-reference**: [user tutorial](../design/user/user-tutor.md) and
> [user architecture](../design/user/user-arch.md).

---

## 5.1 Construct an execution environment

For a configured type-3 task, `create_elf_process()` allocates a TCB and user
root, loads ELF segments and constructs user and kernel stacks. The private
low mappings hold user memory; shared high mappings let kernel code remain
accessible after a privilege transition.

The task's user stack contains arguments using user virtual addresses. The
kernel writes those values through its own mapped aliases. Storing an HHDM
pointer into `argv` would give user code the wrong address domain.

The boot file has a separate embedded-ELF demonstration using the kernel root.
Follow the configured path when studying private address spaces; do not merge
both constructors into one account of process isolation.

## 5.2 First entry differs from syscall return

The initial architecture stub sets up an `iretq` frame with the ELF entry,
user stack and ring-3 selectors. `_start` in `user/crt0.s` reads the argument
layout and calls `main()`.

A later syscall has a real user continuation to restore. Hardware puts that
return RIP in RCX and flags in R11, then enters the configured LSTAR address.
It does not automatically select the thread's kernel stack; assembly does
that explicitly.

## 5.3 Build the syscall frame before enabling interrupts

The entry changes GS, records user RSP temporarily, selects the current
thread's kernel stack and saves the syscall frame. Its 18 words contain the
general registers other than the hardware-clobbered RCX/R11, plus the return
RIP, selectors, flags and RSP.

The exact order is documented in the
[frame layout](../design/user/user-arch.md#complete-syscall-frame). In particular,
R12–R15 have explicit slots. RCX/R11's return values are represented by RIP
and RFLAGS, not by snapshots of their pre-syscall contents.

Only after the frame exists does C enable interrupts. A preemption can then
suspend the syscall without losing its user return state.

## 5.4 Dispatch and return

The dispatcher reads the call number from saved RAX and arguments from saved
registers. It supports write, yield, mmap, munmap, sleep and exit. The current
ABI is small and project-specific; it does not emulate Linux numbering or
full POSIX behavior.

For a returning call, C writes the result into saved RAX, disables interrupts
and drains pending rescheduling. Assembly restores state and executes
`sysret`. A sleeping syscall can therefore resume through the same path
after another task has run.

## 5.5 A frame is not a process model

Copying a syscall frame would not clone the pages it refers to. Fork also
needs a child address space, ownership rules, a child first-run path, distinct
parent/child results and cleanup on partial failure. Waiting for children
and collecting their status require additional lifecycle state.

No fork syscall currently exists. The `ret_from_fork` assembly label is used
for initial thread entry. Similarly, logging an exit request is not evidence
that a parent can wait for it or that memory reclamation has been verified.

## Experiment: follow three user tasks

```sh
make test-x86_64-multiuser
```

The fixture configures three user tasks on one CPU. Follow their load,
sleep/yield, lazy-page access and completion output. The historical completion
marker says `EL0`, although x86_64 executes ring 3.

The target counts that message before `exit()`. It does not check every saved
register with sentinel values or prove final page reclamation. Such checks
would be additional tests, not conclusions from this smoke gate.

**Previous**: [Scheduling](04-scheduler.md). **Next**: [Devices](06-devices.md).
