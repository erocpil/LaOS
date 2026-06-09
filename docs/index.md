# LaOS Documentation

LaOS is a teaching kernel for x86_64. Start with the documents below;
process notes and old reviews are supporting evidence, not the authoritative
description of the current tree.

## Start here

- [Getting started](getting-started.md) — dependencies, first build, tests and troubleshooting.
- [Test guide](testing-guide.md) — one entry point for every maintained test method and its detailed documentation.
- [Current limitations](current-limitations.md) — current capability boundary.
- [Task configuration DSL](task-conf-dsl.md) — modules, users and selftests declared in `task.conf`.

## Design and implementation notes

- Storage subsystem:
  - [Tutorial: follow a LaFS read to virtio-blk](design/storage/storage-tutor.md)
  - [Architecture: contracts, invariants and limits](design/storage/storage-arch.md)
- Kernel modules:
  - [Tutorial: build, relocate and run a module](design/module/module-tutor.md)
  - [Architecture: loader transaction and ABI boundary](design/module/module-arch.md)
- Testing:
  - [Entry: find the right test and its documentation](testing-guide.md)
  - [Tutorial: choose and interpret LaOS tests](design/testing/testing-tutor.md)
  - [Architecture and coverage matrix](design/testing/testing-arch.md)
- RCU:
  - [Tutorial: readers, grace periods and reclamation](design/sync/rcu-tutor.md)
  - [Architecture: state, hooks, ordering and test boundary](design/sync/rcu-arch.md)
- Scheduler:
  - [Architecture: fixed priorities, queue migration and switch-layout ABI](design/scheduler/scheduler-arch.md)
- Mutex:
  - [Architecture: raw/handoff policy, ordered waiters and priority inheritance](design/sync/mutex-design.md)
- TTY and monitor:
  - [Tutorial: framebuffer pages, monitor views and input](design/tty/tty-tutor.md)
  - [Architecture: rendering, visibility and platform boundary](design/tty/tty-arch.md)
- Diagnostics and exceptions:
  - [Tutorial: logs, fault decoding, symbolisation and panic](design/diagnostics/diagnostics-tutor.md)
  - [Architecture: exception routing and crash-path limits](design/diagnostics/diagnostics-arch.md)
- [VMM review](vmm-review.md)

## Historical process records

Files named as reviews, fix summaries, plans or dated discussions are snapshots
of a particular development stage. They may mention removed paths or completed
limitations. Use [current limitations](current-limitations.md) for current
status and Git history when exact provenance matters.

- [Resolved issues and review archive](known-issues.md)
- [Coding style](coding-style.md)
