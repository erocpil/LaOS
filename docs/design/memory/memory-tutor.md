# Memory tutorial

[中文版](memory-tutor-zh.md)

Three different questions recur in memory code: who owns a physical page,
which virtual address maps to it, and which virtual ranges a user task has
reserved. PMM, VMM and VMA tracking answer those questions separately.

## 1. Trace a physical allocation

Read `pmm_alloc()` in `kernel/pmm.c` and follow it into the bitmap scan.
Then find a caller that uses `phys_to_virt()` before writing to the result.
The conversion provides a CPU-accessible alias; it does not change ownership.

For a newly allocated page, ask who initializes it and who eventually calls
`pmm_free()`. Do not infer zero filling from the name of the allocator.

## 2. Walk one translation

Use `vmm_get_phys()` in `kernel/vmm.c` to follow the four x86_64 page-table
levels. Each index selects nine address bits; a normal page leaves twelve
bits for the byte offset. Check the large-page cases before assuming every
walk reaches a 4 KiB leaf.

Compare the root arguments of `vmm_map()` and `vmm_map_user()`. One expects
a virtual pointer and the other a physical root address. Passing the wrong
domain can fail only after switching address spaces, which makes the error
look like a scheduler bug.

## 3. Reserve without allocating

The following pattern already appears in `user/main.c`:

```c
char *buf = mmap(8192, PROT_READ | PROT_WRITE, MAP_LAZY);
if (!buf)
    exit(1);
buf[0] = 'A';
buf[4096] = 'B';
munmap((unsigned long)buf, 8192);
```

After mmap, one VMA covers two pages. The first write to each page faults
because its translation is absent. The handler supplies a page and retries
the instruction. This example intentionally writes before reading: current
anonymous allocation does not guarantee zero-filled contents.

## 4. Follow release in the opposite direction

Read `do_munmap()` in `kernel/arch/x86_64/syscall.c`: locate the exact VMA,
walk each mapping, unmap the page, free its physical storage, then remove
the VMA record. An untouched lazy page has no physical allocation to free.

Compare this with an eager mmap that fails after allocating some pages.
The current failure return does not perform the corresponding full unwind.
Recording that difference is essential when reviewing future memory work.

## 5. Run the existing evidence

```sh
make test-x86_64-multiuser
make test-x86_64-smp-tlb
```

In the multi-user log, find `[P4-4] demand paging OK` and the second-page
message. The target itself counts the user completion marker, so read the
intermediate output if demand paging is what you are investigating.

The 512 MiB `MAP_LAZY` request in the user demonstration reserves virtual
space without touching all pages. Its success or failure is not a physical
OOM test. QEMU memory size and virtual-range availability are separate
constraints.

## 6. Exercise: account for two pages

For the example above, describe the state after mmap, after each write and
after munmap. Count VMA records and user data pages separately; page-table
pages are a third category. Explain why one successful mmap return cannot
predict the number of physical pages currently owned by the task.

Continue with the [memory architecture](memory-arch.md) and
[Chapter 5: User Mode](../../book/05-user-mode.md).

## 7. Exercise: separate mapping visibility from page ownership

For one lazy page, record the PMM owner, VMA record, page-table entry, TLB
state and any DMA/software references before and after a fault and unmap. State
which event permits PMM reuse. A cleared PTE alone is not evidence that every
CPU has discarded its translation.
