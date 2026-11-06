# Chapter 8: Porting

[中文版](08-porting-zh.md)

> **Prerequisites**: Chapters [1](01-boot.md)–[7](07-synchronization.md).
> **You'll build**: a list of architectural contracts and evidence for a port.
> **Cross-reference**: [branch strategy](../process/branch-strategy.md) and
> [multi-architecture strategy](../process/multi-arch-strategy.md).

---

## 8.1 Separate shared policy from machine operations

Scheduling policy, task parsing, module registration and LaFS format handling
are shared concerns. Interrupt registers, page-table encodings and privilege
return instructions belong to an architecture implementation.

`kernel/arch_dispatch.h` selects architecture headers. This central selection
point helps expose the contracts, but does not mean the rest of the tree is
free of architecture conditionals or historical naming.

Names such as `pml4_phys` also survive in shared interfaces. When implementing
another page-table format, preserve the physical-root contract instead of
assuming the name requires x86_64 hardware.

## 8.2 A contract includes ordering and lifetime

| Contract | x86_64 example | What a port must preserve |
| --- | --- | --- |
| IRQ state | save/restore RFLAGS interrupt state | restore the caller's previous state |
| Context switch | kernel stack plus FPU save area | suspended continuation and register ownership |
| User transition | first `iretq`, syscall `sysret` | correct privilege, stack and return context |
| Page-table update | PTE write and TLB invalidation | visibility before reuse or resumed access |
| Remote wakeup | queue publication and reschedule IPI | target observes the published runnable work |
| DMA transition | descriptor/buffer synchronization | device and CPU agree on ownership and contents |

A function with the same signature can still violate the contract by
omitting a barrier or restoring interrupts at the wrong time. Architecture
parity therefore requires runtime evidence as well as compilation.

## 8.3 Build the port in observable stages

Start with reliable early output and boot metadata, then physical allocation
and mappings, local exceptions and timer, one schedulable thread, and user
entry/return. Add SMP only after the local continuation and stack model are
clear. Device transport tests then exercise DMA and routing independently
of filesystem or packet-format tests.

Each stage should have a concrete observation: a returned syscall, a second
CPU's acknowledgement, or data read through the real transport. A boot banner
cannot substitute for those observations.

## 8.4 Respect branch contents

This checkout contains the x86_64 implementation. ARM64 implementation and
its direct/UEFI-Limine boot targets live on `arm64`; their coverage differs.
The experimental RISC-V directory is not a feature-parity promise.

The documented shared-code workflow starts on `x86_64`, validates the change
there, and rebases ARM64 before running its relevant gates. Consult the
[branch strategy](../process/branch-strategy.md) before changing shared code.
Historical port reviews are evidence of their recorded stage, not the current
capability boundary.

## Exercise: audit one interface

Choose page-table-root switching or IRQ save/restore. Read the x86_64 header
and all relevant callers. Record the input address domain, local interrupt
state, required ordering and permitted execution contexts.

Use the [coverage matrix](../design/testing/testing-arch.md#coverage-matrix)
to choose a test for each property. Mark properties without a test explicitly;
the presence of a build target does not establish that every property is
asserted by it.

**Previous**: [Synchronization](07-synchronization.md).
**Next**: [Walkthrough](09-walkthrough.md).
