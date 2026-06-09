# Current Limitations

This document is the authoritative user-facing summary of LaOS limitations.
Historical defects and completed fixes remain available from the
[documentation index](index.md); their path differs after ARM64 process
documents are grouped under `docs/process/`.

## Architecture and platform

- Automated coverage is QEMU-focused; real hardware compatibility is not
  established.
- On x86_64, the BSP configures TSS IST stacks for fatal exceptions, but AP
  IST setup is incomplete. Ordinary IRQs also execute on the interrupted
  thread's kernel stack; per-CPU IRQ stacks are allocated but not yet used for
  frame switching.
- This working tree contains only the x86_64 architecture implementation.
  `test-arm64` and `test-riscv64` are retained as placeholders and explicitly
  skip when their architecture directories are absent; they do not provide
  cross-architecture validation.

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
