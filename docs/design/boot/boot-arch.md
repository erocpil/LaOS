# Boot architecture

[中文版](boot-arch-zh.md)

## Scope

This document follows the x86_64 Limine path in the current tree. ARM64 boot
implementation lives on the `arm64` branch; its direct-boot and Limine paths
must be evaluated separately. See the [boot tutorial](boot-tutor.md) for a
guided reading and [Chapter 1](../../book/01-boot.md) for the larger sequence.

## Source map

| File | Responsibility |
| --- | --- |
| `kernel/arch/x86_64/main.c` | Limine requests, BSP entry, AP entry and boot task setup |
| `kernel/boot_limine.c` | convert Limine responses to shared boot records |
| `kernel/boot_info.h` | memory-map and module handoff format |
| `kernel/hhdm.h` | direct-map address conversion |
| `kernel/arch/x86_64/cpu.c` | per-CPU state and online coordination |
| `kernel/task.c` | construct and release configured tasks |

The entry is `kmain()`, selected by the x86_64 linker script. Boot information
comes from the response fields of static requests in `.limine_requests`.
There is no single boot-info argument passed to `kmain()`.

## Boot information ownership

`check_bootloader()` checks the base revision and required framebuffer,
HHDM and memory-map responses. It initializes the direct-map offset before
passing responses to `boot_info_from_limine()`.

The adapter copies memory-map entries and module descriptors into static
arrays, currently limited to 128 entries and 64 modules. Module contents,
paths and command strings remain referenced through pointers; this is not
a deep copy of bootloader memory. Reclaiming bootloader-owned storage would
require an explicit lifetime audit of those pointers.

CPU count is capped at `MAX_CPUS`. Logical CPU indices come from the Limine
CPU array and must not be confused with potentially sparse LAPIC IDs.

Boot records have borrowed-storage boundaries. The copied array entries are
owned by LaOS, while module payloads, paths and command strings remain owned by
the bootloader through pointers. Any future boot-memory reclamation must first
replace those references with deep copies or prove that every consumer has
stopped using them.

## BSP sequence

The following groups preserve the order in `kmain()`:

1. Disable local IRQs, establish early GS, initialize logging, check boot responses, initialize TTY and enable FPU.
2. Establish GDT/TSS and IDT; mask the legacy PIC.
3. Initialize PMM, the kernel page-table root, heap and module allocator.
4. Map LAPIC and PCI configuration space; initialize LAPIC, I/O APIC and
   syscall MSRs.
5. Run synchronous VMA, CPIO, LaFS, block-device and x86 user-loader checks. Clear mock device,
   CPIO and mount state before PCI enumeration and real virtio-blk setup.
6. Discover the e1000 interrupt line; establish the BSP context, idle and main thread before publishing AP entries.
7. Select base, test or explicit compatibility mode and join the online barrier. Test mode has separate base and test contexts.
   Only test/compatibility mode starts runtime selftests.
8. Release configured tasks, enable interrupts
   and enter the monitor/main scheduling loop.

This is a dependency sequence, not a claim that every stage has transactional
failure recovery. For example, a missing required boot response panics;
absence of a virtio block device prints a skip message instead.

The sequence has three useful gates:

```text
bootloader contract accepted -> kernel memory/CPU state established
                              -> devices and tasks initialized
                              -> interrupts enabled and scheduling released
```

Enabling interrupts before the corresponding IDT, controller and per-CPU
state are ready can turn an ordinary asynchronous event into an early-boot
failure. Starting a task before its allocator, page-table root or scheduler
state is ready has the analogous ownership problem.

## Shared phase completion contract

`kernel/boot_phase.h` defines the same logical boundaries for x86_64 and the
ARM64 branch. Each BSP completes them in this order with local IRQs masked:

```text
memory -> bootstrap-tests -> devices -> runtime -> smp -> services
```

| Boundary | Completion condition |
| --- | --- |
| `memory` | CPU context, memory allocation and platform exception/interrupt setup usable |
| `bootstrap-tests` | VMA/CPIO/LaFS/block-device fixtures passed and shared fixture state cleared |
| `devices` | device registration/probing completed or explicitly skipped for this boot path |
| `runtime` | boot/idle contexts, run queues and task configuration ready |
| `smp` | all expected APs reported local readiness through the architecture barrier |
| `services` | BSP task release and configured-selftest startup completed |

`boot_phase_complete()` rejects a skipped/repeated transition or completion
with IRQs enabled. It prints `[boot] <phase> complete` before publishing the
phase with release ordering. Online APs observe `services` with acquire
ordering before releasing configured work. The existing online rendezvous
must finish first, so the BSP never waits for work blocked on its own service
publication. An AP timeout cannot count as successful SMP completion.

The stateful unit fixtures run only between `memory` and `bootstrap-tests`,
before PCI/real block registration and real initrd loading. Cleanup clears
the mock registry, CPIO archive/cursor and LaFS mount metadata. Runtime
`@test vma`, `@test cpio`, `@test lafs` and `@test block_device` entries report
cached results; they cannot replace a live filesystem or unregister devices.
Failure of the early checks stops boot. Details and the host-side marker
oracle are in [Testing architecture](../testing/testing-arch.md#startup-dependencies-and-completion-conditions).

The ARM64 direct path parks APs before platform setup; the Limine path adopts
firmware mappings and performs AP GIC setup separately. These machine-specific
steps remain distinct. Limine's explicit PCI skip can complete `devices`
without claiming storage coverage. Logical completion points do not require
identical machine-initialisation calls across architectures.

## AP sequence

`start_smp()` assigns `secondary_cpu_init` to each secondary CPU's Limine
`goto_address`. The AP establishes GS state, adopts the kernel page-table
root, loads its GDT/TSS and IDT, initializes LAPIC and per-CPU state, and
programs syscall MSRs. It joins `wait_online()`, waits for `services`, releases
its tasks, enables interrupts and schedules.

Per-CPU setup is necessary even when an object such as the IDT table is
shared: loading a descriptor register or programming an MSR affects the
executing CPU. A successful BSP boot does not establish AP readiness.

AP readiness is a local contract: the AP must have valid GS, descriptor tables,
interrupt-controller state, page-table root and scheduler state before it is
counted by `wait_online()`. Shared objects such as an IDT table do not remove
the need for per-CPU register and MSR setup.

Before publishing AP entries, the BSP establishes its main and idle threads.
Each AP uses a separate bootstrap TCB. Both entries call
`cpu_assert_sched_context_ready()` before `sti` to check context identity,
thread ownership, state and stacks. The main thread remains RUNNING and resident
in the runqueue, as required by the current scheduler for subsequent selection.
These checks do not replace architecture register/controller initialization.

## Boot file sets and configuration contexts

x86_64 defaults to a standalone initrd base configuration. `boot=test` adds a
separate test context; `boot=compat` explicitly selects the legacy Limine
collection. Sources never substitute for each other. A rescue purpose exists in
the context API but has no separate boot-mode entry. Implementation history is
recorded in [the boot payload plan](../../process/boot-payload-refactor-plan.md).

`boot_files.h` defines borrowed `boot_file` views and explicitly scoped file
sets. Adapters accept a bootloader-independent module list or a CPIO archive.
Lookup normalises leading/duplicate slashes and `.` components, rejects `..`,
source-qualified names and excessive lengths, and compares full paths exactly.
Bare task/payload names resolve under `/task/`; configuration lookup uses an
explicit path. Missing, invalid and ambiguous files are distinct outcomes.
One set never searches another set, and duplicate canonical names fail lookup.
CPIO directories are skipped; symlinks and other special entries are rejected.

`task_conf_init_ctx()` binds a zero-initialised context to a borrowed file set
and purpose: base, test, recovery or compatibility. Tasks, selftest directives,
missing-module policy, selected configuration path and per-CPU submission
state belong to that context. Base/recovery contexts reject `@test`;
test/compatibility contexts can record it. Parse failure frees partial records
and resets policy. A successful context cannot be parsed twice, and a started
context cannot be destroyed or reinitialised. The caller serialises execution
claims; the task launcher uses `task_lock` and rejects repeat CPU submissions.

`task_init_ctx()` and `task_run_configured_cpu_ctx()` are the explicit APIs.
The x86_64 entrypoint and stats display share the default context, explicitly
initialized with base or compatibility purpose. Test mode adds a separate
`test_conf` bound to the standalone Limine test collection. Legacy adapters remain for
architecture paths that have not yet migrated. Selftest payload lookup uses
the selected context's file set; its runtime engine remains single-session,
so a second selftest session is rejected rather than sharing mutable test state.
Multiple independent parsed configs do not imply concurrent test execution.

`cpio_reader` provides independent archive cursors. The old `cpio_init/next/find`
APIs wrap a default reader for existing early tests.
The packer and parser now align header plus filename to a four-byte boundary
as required by newc, rather than padding the filename alone. Regenerate old
archives with `script/mkcpio.py`; the previous nonstandard layout is not a
second supported format. Metadata ranges, hex fields, names and truncation
are checked before exposing file data. These are borrowed views, so the caller
must retain source descriptors, paths and payload memory until all consumers
have stopped. There is no bootloader-memory reclamation or VFS in this phase.

`make test-boot-context` compiles the real file adapters, CPIO parser and config
parser on the host with AddressSanitizer/UndefinedBehaviorSanitizer. It checks
source/cursor isolation, exact lookup, duplicate names, malformed/truncated
archives, context purpose, allocation-failure cleanup and execution claims.
QEMU gates separately check actual task loading and runtime behaviour.

## Configured user-program construction

Base, test and compatibility type-3 tasks use `create_elf_process()` and receive
private user page-table roots. The task layer names and enqueues each thread.
The former hand-written embedded-ELF loader and its unqueued thread are removed
from x86_64. Source selection changes the file collection, not the user loader.
See the [user architecture](../user/user-arch.md) for ownership and entry rules.

Before devices are registered, `test_user_loader_run()` constructs and destroys
unqueued synthetic user processes. After warming heap growth, four repeated
success/invalid-stack-argument cycles must restore CR3, free physical-page count
and live heap bytes. The failure occurs after segment mapping, so this checks
partial address-space cleanup. It does not inject every allocator failure or
prove eventual zombie reclamation after runtime exit.

## Validation and limits

```sh
make test-x86_64
make test-x86_64-smp-tlb
```

The normal target checks the running marker and named selftest results; the
TLB target adds focused cross-CPU translation evidence. Neither establishes
general firmware compatibility or arbitrary CPU topology support. Use the
[test matrix](../testing/testing-arch.md#coverage-matrix) to select additional
gates instead of treating the running marker as complete boot validation.

Boot evidence should be recorded by phase: boot response acceptance, memory
ownership, BSP architecture setup, AP online completion, device discovery,
task construction and final interrupt/scheduler entry. The final running
marker proves only that the selected path reached the monitor; it does not
prove that every optional device or user task completed successfully.

## Standalone base initrd packaging

`make initrd` builds `build/initrd-x86_64.cpio` and a JSON manifest from
`conf/task-base.conf`, `user.elf`, `module_foo.mo` and `e1000.mo`. The explicit
component list excludes test payloads. The default configuration starts the
user and worker on CPU0; e1000 is available but has no automatic startup entry.
`INITRD_TASK_CONF` selects another base configuration.

The builder normalizes archive paths, rejects duplicates and checks ELF64
architecture/type headers. Before publishing, a host checker compiled from
the real CPIO, boot-file and task-config implementation loads `/conf/task.conf`
as `TASK_CONF_BASE`: test directives, invalid syntax, ambiguous paths and missing
references fail even when the configuration requests skipping missing modules.
This validates packaging and references, not relocations or runtime readiness.

Entries have stable ordering, ownership and inode numbers. Modification time is
zero by default or `SOURCE_DATE_EPOCH`. The manifest records per-file size/hash,
ELF type/machine and the archive hash. Validation failure preserves previous
outputs. `make test-initrd` checks determinism, byte integrity and rejected inputs.

## External initrd boot

`make iso-base` builds `build/base/LaOS.iso`. Limine delivers the complete archive
as `/boot/initrd.cpio`; the kernel borrows that memory for the lifetime of the
configuration and payloads. No bootloader memory is reclaimed. `boot=base` (also
the absent/empty command-line default) accepts only the archive's `/conf/task.conf`
and components. Missing or ambiguous archives, malformed CPIO, invalid base
configuration, missing component references and unavailable target CPUs stop boot.
Base mode accepts executable driver/thread/user tasks and rejects test directives.
A present component that fails loading is logged and skipped; later tasks continue.

`boot=compat` explicitly enables the existing Limine config/module collection.
Unknown command lines are rejected. `make` / `make iso` / `make run` now default
to base mode; compatibility requires `X86_64_BOOT_MODE=compat`. The original
`conf/task.conf` is retained for legacy/ARM64 paths. No mode substitutes files
from another source.
x86_64 no longer depends on the embedded header; `embedded-initrd` remains a
migration target for architectures that still require it.

`make test-x86_64-initrd` checks base selection, actual queueing, worker progress,
user syscall exit, three distinct live user roots, safe rejection of a truncated
ELF followed by successful tasks, and fatal missing/corrupt/config/test-directive
cases. A missing reference is also tested with a matching standalone Limine
component present: it must still fail. Logs are under `build/base/`, including
per-case serial/QEMU/build logs under `cases/`. This is x86_64 QEMU TCG evidence;
ARM64 and other bootloaders are not covered.

## Explicit test mode

`make iso-test` emits `boot=test`, a base archive and `/conf/test.conf` selected
by `X86_64_TEST_CONF` (default `conf/test.conf`). The x86_64 focused fixtures use
`conf/test-x86_64-*.conf`. Their grammar matches `task.conf`; purpose and file
collection determine semantics. Test mode requires a valid base archive and a
valid test configuration. It does not choose one configuration over the other.

The global base context retains its tasks and policy. A separate test context
holds test directives, fixture tasks and CPU submission bits. The test collection
excludes the transport archive. After SMP setup, only the test context is passed
to the single-session selftest engine. Each CPU submits base tasks and test tasks
once, against their own source collections. Queue submission is not service
readiness; tests needing device or service readiness must still establish it.

Standalone user/worker images in test mode are regression fixtures, including
same-name copies independent of the base components. Normal images contain only
the kernel, archive and bootloader files. Rebuilding clears the ISO staging tree
so switching from test to base does not retain test payloads. `make test-x86_64`
and specialized gates explicitly select test mode. The multiuser gate requires
all three fixture exits as well as the base user, so the extra base task cannot
satisfy a missing fixture result.

`make test-x86_64-boot-modes` boots differently marked same-name ELF copies and
checks independent execution, ignored test configuration in base mode, fatal
missing test configuration, no test-component fallback to initrd, and the base
archive requirement in test mode. It also rebuilds test then default base mode
and inspects the actual ISO file list. This does not provide concurrent selftest
sessions, live config reload, or migration of every built-in test to `.mo`.
