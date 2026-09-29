# Memory architecture

[中文版](memory-arch-zh.md)

## Scope and ownership

LaOS separates physical allocation, page-table operations, kernel heap
allocation and user virtual-range tracking. This note describes the current
x86_64 path. It does not imply a complete POSIX virtual-memory subsystem.

| Component | Owns | Does not establish |
| --- | --- | --- |
| `kernel/pmm.c` | physical-page bitmap and accounting | virtual mappings or zero-filled pages |
| `kernel/vmm.c` | page-table construction and updates | user range policy |
| `kernel/heap.c` | kernel allocation blocks | user allocation lifetime |
| `kernel/vma.c` | per-thread sorted virtual ranges | resident physical pages |
| `kernel/arch/x86_64/page_fault.c` | user fault classification and demand mapping | copy-on-write |
| `kernel/arch/x86_64/syscall.c` | mmap/munmap syscall policy | general mapping transactions |

## Physical pages and address domains

PMM begins with a used bitmap, releases usable memory-map ranges, and keeps
its own bitmap storage and protected ranges reserved. Allocation uses a
first-fit scan under one global IRQ-safe lock. `pmm_alloc_pages()` requests
a contiguous run; it is not a scatter/gather allocator.

The return value of `pmm_alloc()` is a physical address encoded as a pointer.
PMM does not clear the page. A caller that needs initialized bytes must clear
them through a valid kernel mapping before publishing the page.

These API distinctions matter even though several arguments use pointer types:

| Value or operation | Address domain |
| --- | --- |
| `kernel_pml4` | kernel virtual pointer to the root |
| `thread->pml4_phys` | physical root address |
| `vmm_map()` / `vmm_unmap()` root argument | kernel virtual pointer |
| `vmm_map_user()` root argument | physical address |
| `vmm_get_phys()` root argument | physical address |
| `phys_to_virt()` | physical address to HHDM alias |

`virt_to_phys()` is subtraction of the HHDM offset. It is not a page-table
walk and is not valid for an arbitrary heap, module or user virtual address.

The address-domain rule is also an ownership rule:

```text
PMM allocates physical page -> caller initializes through a kernel alias
                            -> VMM publishes a translation
                            -> CPU/user code may access the mapped address
                            -> unmap/invalidate -> PMM may reclaim the page
```

The physical address in a DMA descriptor is not a user virtual address and
not necessarily the value of an arbitrary kernel pointer. A page must not be
returned to PMM while a page table, DMA device or software queue still refers
to it.

## Kernel and user roots

On the BSP, `vmm_init()` allocates a root, preserves the existing low entry
when present, and copies Limine's high-half entries. Lower-level mappings
referenced by copied entries remain shared. APs adopt this kernel root.

Kernel high-half root entries are preallocated so later lower-level kernel
mappings can be visible through user roots that already copied those entries.
`vmm_create_user_pml4()` clears a new root and copies entries 256 through 511
from `kernel_pml4`; user mappings occupy the low half.

This is shared kernel mapping structure, not a copy of every physical page.
Destruction must free private user mappings without reclaiming shared kernel
subtrees. The embedded boot demonstration also uses the kernel root directly;
see the [user architecture](../user/user-arch.md).

## VMA and residency

A VMA records `[start, end)`, protection flags and mapping flags in a
per-thread sorted list. It answers whether a range is reserved. A page-table
walk answers whether a particular page currently has a translation.

```text
mmap without MAP_LAZY -> reserve VMA -> allocate/map pages
mmap with MAP_LAZY    -> reserve VMA -> return virtual address
                                      -> first access faults -> allocate/map page
```

The syscall ABI is `mmap(length, prot, flags)`. It has no address or file
descriptor argument. The existence of constants such as `MAP_FIXED` does
not mean the Linux mapping contract is implemented.

The x86_64 `munmap` handler requires the rounded range to match one complete
VMA. `vma_free()` likewise requires an exact match; neither path splits a VMA
to release an arbitrary subrange.

## Demand-fault path

The handler reads CR2, separates user faults from kernel faults, and looks up
the current thread's VMA. A not-present user fault inside a VMA allocates a
page and installs user, writable and NX flags derived from that VMA.
Protection faults terminate the user thread; kernel faults use diagnostics.

Current implementation boundaries are significant:

- demand pages are not explicitly zeroed, and PMM supplies no zeroing guarantee;
- the handler does not comprehensively validate access against `PROT_READ`
  before allocating a not-present page;
- eager mmap failure returns zero without unwinding all prior mappings and
  the already inserted VMA;
- syscall range rounding and allocation failures are not comprehensively
  hardened;
- no COW, swap, page cache or shared address-space reference model exists.

These are limits of the implementation, not behavior that applications
should rely on. They matter before treating this teaching path as isolation
for untrusted programs.

The fault path has three decisions that should remain separate:

1. classify the fault as user or kernel and read the faulting address;
2. find an exact VMA and derive the intended page permissions;
3. allocate, initialize and publish a translation, or terminate the task.

Repairing a not-present fault is not the same as accepting a protection fault.
The current implementation's incomplete permission and rollback checks are
therefore correctness boundaries, not merely performance limitations.

## Mapping updates and SMP

VMM serializes updates with `vmm_lock`. Unmap clears the translation and
performs local invalidation; its wrapper requests remote TLB invalidation
after releasing the lock. Read both the update and IPI paths when changing
reclamation: sending an interrupt and proving completion on every relevant
CPU are different events.

The maintained remap tests exercise repeated visibility across CPUs. They
do not prove arbitrary address-space teardown or a general page-reuse
protocol under every interleaving.

The TLB protocol has two completion points:

```text
update page table -> invalidate local translation
                  -> request remote invalidation
                  -> receive remote handler
                  -> invalidate remote translation
                  -> return to code that may reuse the page
```

Page reuse before the remote completion point can expose a stale translation
to another CPU. The remap test checks a bounded instance of this protocol; it
does not establish a universal teardown barrier for every address space.

## Validation

```sh
make test-x86_64
make test-x86_64-smp-tlb
make test-x86_64-multiuser
```

Inspect the VMA unit-test and demand-paging output as well as the target's
exit status. The multi-user target counts completion messages; it does not
check zero-filled anonymous pages or full allocation rollback. See the
[memory tutorial](memory-tutor.md) for a focused reading exercise.

Interpret evidence by layer: VMA tests cover range bookkeeping, demand-paging
markers cover a fault-and-retry path, and SMP remap tests cover a bounded TLB
visibility sequence. None proves zero-fill, COW, swap, complete mmap rollback
or arbitrary page-reuse synchronization.
