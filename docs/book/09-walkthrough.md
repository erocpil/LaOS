# Chapter 9: Walkthrough

[中文版](09-walkthrough-zh.md)

> **Prerequisites**: the preceding chapters and a working build environment.
> **You'll build**: one explanation supported by source, configuration and logs.
> **Cross-reference**: [test guide](../testing-guide.md) and
> [testing architecture](../design/testing/testing-arch.md).

---

## 9.1 Choose a bounded question

Use this question: how does a configured user task access a lazy page, sleep
and return to user mode? The question spans several subsystems but has a
specific execution path and an existing fixture.

Start from `conf/task-x86_64-multiuser.conf`. It requests three user tasks
on CPU 0. Read the target recipe as well as the fixture: the recipe chooses
QEMU memory, CPU count, timeout and the completion assertion.

## 9.2 Follow construction

```text
task configuration -> create_elf_process
                   -> private user root / ELF segments / argument stack
                   -> thread kernel stack / initial switch frame
                   -> enqueue -> first ring-3 entry
```

For each arrow, identify the function that consumes the preceding object's
state. Keep the embedded boot demonstration separate because it does not
use the same private-root constructor.

## 9.3 Follow one lazy write

In `user/main.c`, mmap returns an address for an 8192-byte lazy range. Before
the first access, find the VMA and explain why no data page is needed yet.
Then follow the write through vector 14, VMA lookup, allocation and mapping.

The repaired fault returns to retry the user instruction. Later `write()`
enters through the syscall path to print data; it does not reuse the page
fault's interrupt frame. This connects the two return paths without treating
their layouts as interchangeable.

## 9.4 Follow suspension and completion

For sleep, follow the wrapper to `schedule_timeout()`. Explain which kernel
stack retains the syscall continuation while another user task runs. On
resumption, the handler updates saved RAX and returns through `sysret`.

The program prints its completion marker and then calls exit. Those are
two separate events. The test's marker count observes the former, while
successful destruction of all resources would require additional evidence.

## Experiment: collect only the relevant gates

```sh
bash script/check_doc_links.sh
make test-task-conf-v1
make test-x86_64-multiuser
```

The first validates documentation references. The second checks committed
configuration grammar. The third runs the selected kernel scenario. Keep
their claims separate: neither host-side check executes the kernel parser
or verifies register restoration.

Before running another QEMU target, retain the relevant log if comparing
results: several targets reuse `build/serial.log`. A later target can replace
the evidence from an earlier run.

## 9.5 Read a failure at the correct layer

| Observation | First evidence to inspect |
| --- | --- |
| Build fails | compiler/linker output and generated offset dependencies |
| No kernel output | QEMU startup error, ISO contents and boot handoff |
| User ELF never reaches its marker | task configuration, loader and first-entry frame |
| Lazy access terminates | CR2, error code, VMA and page-table flags |
| Task stops after sleep | state, wakeup tick, runqueue and interrupt progress |
| Completion count too small | per-task output, timeout and preceding exceptions |

The first missing expected observation narrows the search. It does not
prove the immediately preceding subsystem caused the failure; inspect the
state at that boundary before changing code.

## 9.6 Write a reviewable conclusion

A useful report identifies the branch/commit, fixture, command, asserted
result and remaining limits. For this walkthrough, multiple completion
messages support repeated user execution. They do not prove fork support,
zero-filled anonymous memory, malformed-input safety or leak-free exit.

Return to the [book contents](README.md) or the
[documentation index](../index.md) to choose another subsystem trace.
