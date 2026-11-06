# Chapter 1: Boot

[中文版](01-boot-zh.md)

> **Prerequisites**: C, basic assembly and the distinction between physical and
> virtual addresses.
> **You'll build**: a source-level map from Limine's handoff to schedulable work.
> **Cross-reference**: [boot tutorial](../design/boot/boot-tutor.md) and
> [boot architecture](../design/boot/boot-arch.md).

---

## 1.1 The handoff is a starting state

Firmware and Limine load the kernel ELF and establish the execution state
needed to enter it. LaOS begins at `kmain()` in `kernel/arch/x86_64/main.c`.
The code already has mappings and a usable stack, but still needs to decide
which physical pages it owns, how interrupts enter, and which work may run.

Boot information is supplied through response pointers in static Limine
request structures. `check_bootloader()` checks those responses; it does not
parse a conventional boot-info argument passed in RDI.

## 1.2 Addressability is not allocation

The HHDM response supplies the direct-map offset. Initializing that offset
makes address conversion available. The memory-map adapter then copies the
region descriptions used by PMM.

PMM's bitmap answers whether a physical page is available. The bootloader's
mapping answers whether a CPU address can reach it. Allocating a page that
happens to be mapped but is reserved would still corrupt its owner.

After PMM, VMM establishes the kernel root and the heap can provide smaller
allocations. This order makes memory available for later per-CPU structures,
modules and threads.

## 1.3 Prepare for asynchronous execution

GS state identifies the current CPU. GDT/TSS supplies segment and stack
state; IDT supplies interrupt entry points. LAPIC and I/O APIC setup connects
interrupt sources to those entries.

The BSP establishes early descriptor state before memory initialization,
then configures interrupt hardware after mappings exist. Reading the actual
sequence is more useful than a generic rule that all interrupt setup happens
at one point.

## 1.4 Bring the other CPUs to the same boundary

`start_smp()` assigns each AP's entry through Limine's `goto_address`. The AP
initializes its local registers and per-CPU context, adopts the kernel root,
and joins the online barrier.

The shared IDT table does not remove the need to load IDTR on each CPU.
Likewise, programming syscall MSRs on the BSP does not program them on APs.
Logical CPU indices must also remain distinct from hardware LAPIC IDs.

## 1.5 Construct work, then run it

The boot path runs early unit-style tests, enumerates PCI devices and attempts
the real virtio/LaFS path. Task configuration constructs module and user
work; registered selftests have their own preparation and execution phases.

`task_run()` releases configured tasks and returns. Scheduling calls and
interrupt-return preemption drive later execution. The main thread eventually
starts monitors and repeatedly sleeps rather than leaving all runtime work
inside a single boot routine.

## Experiment: trace initialization evidence

```sh
make test-x86_64
```

Locate the root-switch log, CPU online messages, named selftest results and
`LaOS is running`. For each marker, identify the source function that emits
it. AP log ordering can vary, so compare dependencies rather than requiring
one fixed interleaving.

The boot marker alone does not establish successful user exit or real disk
I/O. Those require their own evidence, introduced in later chapters.

**Next**: [Chapter 2: Memory](02-memory.md).
