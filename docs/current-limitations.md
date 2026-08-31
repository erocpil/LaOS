# Current Limitations

This document is the authoritative user-facing summary of LaOS limitations.
Historical defects and completed fixes remain available under `docs/process/`
from the [documentation index](index.md). They are snapshots rather than the
source of truth for current behavior.

## Architecture and platform

- Automated coverage is QEMU-focused; real hardware compatibility is not
  established.
- x86_64 fatal exceptions use TSS IST stacks, but ordinary IRQs still execute
  on the interrupted thread's kernel stack. Per-CPU IRQ stacks are allocated
  but not yet used for frame switching.
- ARM64 has direct-boot and Limine paths with different feature coverage.
- riscv64 is an experimental skeleton rather than a feature-parity port.

## Memory and process model

- PMM uses a single global lock and a bitmap first-fit allocator.
- There is no swap, page cache, copy-on-write fork or overcommit policy.
- User address spaces support the teaching mmap/munmap and demand-paging path,
  not a POSIX process model.

## Modules

- Module symbols are resolved by name without ABI versioning or signature
  validation.
- Load failure has checkpoint/rollback support, but `module_free()` does not
  implement general unload, reference counting or arbitrary concurrent
  reclamation.

## Devices, storage and networking

- LaFS is read-only.
- virtio-blk is polling-based.
- e1000 supports the project's MMIO/DMA demonstration paths, but there is no
  TCP/IP stack.
- IRQ dispatch is not a general dynamic registration framework; device count,
  routing and affinity assumptions remain deliberately small.
- x86_64 e1000 uses INTx because its MSI delivery path is incomplete.

## Synchronization and observability

- Scheduling uses strict fixed priorities with no aging, load balancing,
  bandwidth control or starvation prevention. Continuously runnable
  high-priority work may indefinitely delay lower-priority work.
- Mutex priority inheritance is single-hop. Donations from several mutexes are
  aggregated for one owner, but boosts do not propagate through nested
  owner/waiter chains and blocked-thread reprioritization is rejected.
- The default runtime gate covers raw mutex policy. Optional handoff policy is
  not a separate hosted CI build variant.
- RCU reclamation is synchronous; `call_rcu`-style asynchronous callbacks are
  not implemented.
- Several diagnostics and live monitors are teaching instrumentation rather
  than stable operational interfaces.

## Test boundary

- Passing QEMU gates demonstrates the listed scenarios, not exhaustive SMP,
  memory-ordering, malformed-input or hardware compatibility coverage.
- Negative, rollback, FPU, multi-user and stress cases are separate targets;
  the basic smoke gate does not imply all of them ran.
