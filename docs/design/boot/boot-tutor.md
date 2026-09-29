# Boot tutorial

[中文版](boot-tutor-zh.md)

A bootloader gives the kernel a running CPU and a description of the machine.
The kernel must turn that starting state into memory ownership, interrupt
handling and schedulable work. This tutorial follows the current x86_64
implementation; the [architecture note](boot-arch.md) records its contracts.

## 1. Find the actual entry

Start with `kmain()` in `kernel/arch/x86_64/main.c`. Read the static Limine
requests above it, then follow `check_bootloader()`. Each request has its own
response pointer. This explains why the kernel can inspect the memory map
without receiving a conventional C argument from the bootloader.

Separate two facts: Limine has already established mappings that allow the
kernel to execute, but LaOS has not yet established its own allocation policy.
Being able to address RAM does not mean that RAM is free to allocate.

## 2. Follow the first usable allocation

```text
Limine HHDM response -> hhdm_init
Limine memory map    -> boot_info_from_limine -> pmm_init_from_memmap
                                             -> vmm_init -> kheap_init
```

PMM tracks physical pages with a bitmap. VMM manages their virtual mappings.
The heap then provides smaller kernel allocations. Read the
[memory tutorial](../memory/memory-tutor.md) before changing this ordering.

`phys_to_virt()` adds the direct-map offset; it does not allocate a page or
create a mapping. `virt_to_phys()` reverses that conversion for direct-map
addresses, not for every kernel pointer.

## 3. Make interrupts meaningful

The early GS base and GDT/TSS establish per-CPU and stack state. The IDT
provides entry addresses. LAPIC and I/O APIC setup determines how interrupt
sources reach those entries. These are separate responsibilities.

Follow the final `sti` in `kmain()` back to the initialization it depends on.
Then compare the AP entry: each CPU has to establish its own machine state
before it can safely run with interrupts enabled.

## 4. Distinguish construction from execution

`task_init()` reads configuration and constructs task records; `task_run()`
releases work on the executing CPU. It returns to its caller. The scheduler
is entered by scheduling calls and interrupt-return preemption, rather than
by a single non-returning `task_run()` loop.

Selftests also have registration, configuration and execution phases. An
entry in the registry is not evidence that a test completed successfully.

## 5. Observe a normal boot

From the repository root, after following [Getting started](../../getting-started.md):

```sh
make test-x86_64
```

Read the serial-log path reported by the target. Locate the page-table root
switch, CPU online messages, selftest results and `LaOS is running` marker.
The exact order of AP messages may vary because CPUs run concurrently.

The target accepts QEMU's timeout exit because this kernel keeps running.
It still requires specific log markers. A timeout alone is neither a pass
nor a useful diagnosis of the last successful initialization step.

## 6. Exercise: explain an initialization dependency

Choose one pair: PMM before heap, per-CPU setup before scheduling, or PCI
enumeration before e1000 initialization. Find the consumer's first access
to the producer's state and explain what value would be missing if their
order were reversed. Do this as a source-reading exercise before attempting
boot-order changes.

## 7. Exercise: classify boot evidence

Choose one normal boot log and classify each marker as bootloader contract,
memory ownership, AP readiness, device discovery, task construction or final
runtime. Explain which markers are prerequisites for the next phase and which
are only optional progress observations.

Continue with [Chapter 2: Memory](../../book/02-memory.md).
