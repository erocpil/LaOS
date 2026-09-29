# User-mode tutorial

[中文版](user-tutor-zh.md)

This tutorial follows a configured user ELF from loading through a syscall
and back. Read the [memory tutorial](../memory/memory-tutor.md) first if a
physical page and a user virtual address still seem interchangeable.

## 1. Select the configured user path

Open `conf/task-x86_64-multiuser.conf`. Its three type-3 records place user
tasks on CPU 0. Follow task construction to `create_elf_process()` in
`kernel/elf_loader.c`, rather than the separate embedded demonstration in
the architecture boot file.

Locate the private page-table allocation, `PT_LOAD` loop, stack construction
and initial architecture frame. Each task needs both a user stack and a
kernel stack. The first contains user call frames; the second holds kernel
execution while serving or suspending that task.

## 2. Follow the first instruction

`arch_user_thread_entry_stub()` builds a five-word return frame and enters
ring 3. At `_start` in `user/crt0.s`, `argc` is taken from the user stack and
`argv` points into the same layout. The stub aligns RSP before calling C.

The sample explicitly calls `exit(0)`. Keep that convention: the current
CRT does not translate a return from `main` into an exit syscall.

## 3. Trace one write

```text
user main -> write wrapper -> syscall
          -> swapgs / thread kernel stack / trap_frame
          -> syscall_handler -> sys_write
          -> saved rax result / register restore -> sysret -> wrapper
```

Compare the wrapper's input registers with the C dispatcher and the saved
fields in `kernel/arch/x86_64/syscall.h`. Inspect RCX/R11 separately: the
hardware uses them for return state, so a wrapper cannot promise to preserve
their pre-instruction values.

`write` accepts only stdout/stderr and caps the count at 4096. It copies
chunks to a kernel stack buffer and prints them as strings. Embedded NUL
bytes and invalid mappings do not have the guarantees of a hosted C library.

## 4. Follow a syscall that sleeps

`msleep()` enters the same frame path, converts milliseconds to ticks and
calls `schedule_timeout()`. While another thread runs, the sleeping thread's
kernel stack preserves the unfinished syscall. When it resumes, the handler
finishes and the same user instruction stream continues.

This is why retaining the frame on a shared per-CPU stack would be wrong.
The frame belongs to a thread even though the entry code obtains that thread
through per-CPU state.

## 5. Observe the existing gate

```sh
make test-x86_64-multiuser
```

Read the log for the loaded marker, lazy-page writes and completion message.
The fixture uses one CPU to make repeated user scheduling easy to observe.
The completion string still says `EL0` on x86_64; it is a historical test
marker, while the actual privilege level here is ring 3.

The target counts that string before the following exit syscall. Do not
interpret the count as a page-leak check or as proof that each final exit
completed successfully.

## 6. Exercise: separate three saved contexts

Compare the syscall trap frame, interrupt frame and `switch_to()` frame.
For each, identify its producer, its stack owner and its return instruction.
Then explain what a future fork implementation would need beyond copying
the syscall frame: page ownership, child construction, two return values
and failure rollback.

Use the exact layout in the [architecture note](user-arch.md) and the narrative
in [Chapter 5](../../book/05-user-mode.md) to check your reasoning.
