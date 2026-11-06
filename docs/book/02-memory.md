# Chapter 2: Memory

[中文版](02-memory-zh.md)

> **Prerequisites**: [Chapter 1](01-boot.md), pointer arithmetic and page alignment.
> **You'll build**: an ownership model for physical pages, page tables and VMAs.
> **Cross-reference**: [memory tutorial](../design/memory/memory-tutor.md) and
> [memory architecture](../design/memory/memory-arch.md).

---

## 2.1 One allocation, several representations

A physical page can appear as an address returned by PMM, an HHDM pointer
used by the kernel, and a low virtual address used by a user task. These
representations can refer to the same bytes while having different uses.

```text
PMM physical page <--- user page-table entry <--- user virtual address
       |
       +--- phys_to_virt ---> kernel direct-map pointer
```

Neither creating another mapping nor adding the HHDM offset creates another
physical allocation. Conversely, unmapping one address does not prove that
all other references to the page have disappeared.

## 2.2 PMM tracks availability

`kernel/pmm.c` uses a bitmap protected by a global IRQ-safe lock. It begins
with pages unavailable and releases suitable usable ranges while reserving
its own bookkeeping. A first-fit scan finds one page or a contiguous run.

PMM does not zero allocations. The ELF loader explicitly clears the loaded
segment's memory before copying file bytes. Demand allocation currently does
not provide the same zeroing guarantee. These are caller policies, not an
implicit property of the physical allocator.

## 2.3 VMM gives addresses meaning

`kernel/vmm.c` walks four-level x86_64 tables and contains separate large-page
cases. Mapping changes a translation; the permissions determine whether
user access, writes and instruction fetches are allowed.

The root API has a practical trap: `vmm_map()` takes a kernel virtual root
pointer, while `vmm_map_user()` takes a physical root address. Both use a
pointer-shaped C type. Check the contract, not just whether the compiler
accepts the argument.

User roots copy the kernel high-half entries. Their user entries are private,
but referenced kernel subtrees remain shared. A destroy walk must respect
that boundary.

## 2.4 A VMA is a reservation

A VMA records a half-open range and protection flags. With `MAP_LAZY`, mmap
creates that record and returns without allocating its data pages.

The first access produces a not-present fault. If the address belongs to a
user VMA, the handler can allocate a page, install a translation and retry.
See [Chapter 3](03-interrupt.md) for how the interrupted instruction resumes.

Current munmap requires an exact VMA match. There is no partial-range split,
COW or file-backed mapping API. Eager-allocation failure also lacks a complete
rollback of previously mapped pages and the inserted VMA.

## 2.5 Reclamation includes translation state

A CPU may retain a translation after the page table changes. Local TLB
invalidation and cross-CPU invalidation requests therefore accompany mapping
updates. A lock protecting the page-table bytes does not by itself remove
cached translations on other CPUs.

When reviewing page reuse, ask both who can still hold a pointer and who can
still translate an old virtual address. These are different forms of access.

## Experiment: reservation versus residency

```sh
make test-x86_64-multiuser
make test-x86_64-smp-tlb
```

Read `user/main.c` alongside the first log. Its two-page lazy mapping touches
each page separately. Account for one VMA and zero, one, then two resident
data pages. Page-table pages are additional allocations.

The 512 MiB lazy request in the same program does not exhaust physical RAM.
The current mmap search window is only `0x04000000` to `0x10000000`; range
rejection is sufficient to explain that request's failure.

**Previous**: [Boot](01-boot.md). **Next**: [Interrupts](03-interrupt.md).
