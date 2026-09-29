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

1. Initialize logging, check boot responses, initialize TTY and enable FPU.
2. Establish early GS state, GDT/TSS and IDT; mask the legacy PIC.
3. Initialize PMM, the kernel page-table root, heap and module allocator.
4. Map LAPIC and PCI configuration space; initialize LAPIC, I/O APIC and
   syscall MSRs.
5. Run synchronous VMA, CPIO, LaFS and block-device checks. Reset the mock
   block registry before PCI enumeration and real virtio-blk initialization.
6. Discover the e1000 interrupt line and start secondary CPUs.
7. Parse boot modules/tasks, initialize the main thread and join the online
   barrier. Register and configure selftests, then start them.
8. Load the embedded user ELF, release configured tasks, enable interrupts
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

## AP sequence

`start_smp()` assigns `secondary_cpu_init` to each secondary CPU's Limine
`goto_address`. The AP establishes GS state, adopts the kernel page-table
root, loads its GDT/TSS and IDT, initializes LAPIC and per-CPU state, and
programs syscall MSRs. It joins `wait_online()`, releases its tasks, enables
interrupts and schedules.

Per-CPU setup is necessary even when an object such as the IDT table is
shared: loading a descriptor register or programming an MSR affects the
executing CPU. A successful BSP boot does not establish AP readiness.

AP readiness is a local contract: the AP must have valid GS, descriptor tables,
interrupt-controller state, page-table root and scheduler state before it is
counted by `wait_online()`. Shared objects such as an IDT table do not remove
the need for per-CPU register and MSR setup.

## Two user-program construction paths

Configured type-3 tasks use `create_elf_process()` and receive private user
page-table roots. `kmain()` also loads a demonstration ELF from embedded CPIO
through its local `load_user_elf()` helper. That thread records the kernel
root in `pml4_phys`.

Keep these paths distinct when reading logs or reasoning about isolation.
Several printed user messages do not, by themselves, prove that every
message came from a separately constructed address space. Details are in
the [user architecture](../user/user-arch.md).

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
