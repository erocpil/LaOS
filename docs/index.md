# LaOS Documentation

LaOS is a teaching kernel for x86_64 and ARM64. The x86_64 branch contains the
shared and x86_64 documentation baseline; the ARM64 branch extends it with its
architecture implementation and narrative chapters.

## Start here

- [Getting started](getting-started.md) — dependencies, first build, tests and troubleshooting.
- [Test guide](testing-guide.md) — maintained test methods and detailed documentation.
- [Current limitations](current-limitations.md) — authoritative capability boundary.
- [Task configuration DSL](task-conf-dsl.md) — modules, users and selftests declared in `task.conf`.
- [Branch strategy](process/branch-strategy.md) — shared-first multi-architecture workflow.

## Design and implementation notes

- Storage: [tutorial](design/storage/storage-tutor.md) and [architecture](design/storage/storage-arch.md)
- Kernel modules: [tutorial](design/module/module-tutor.md) and [architecture](design/module/module-arch.md)
- Testing: [entry](testing-guide.md), [tutorial](design/testing/testing-tutor.md) and [architecture](design/testing/testing-arch.md)
- RCU: [tutorial](design/sync/rcu-tutor.md) and [architecture](design/sync/rcu-arch.md)
- Scheduler: [architecture](design/scheduler/scheduler-arch.md)
- Mutex: [architecture](design/sync/mutex-design.md)
- TTY and monitor: [tutorial](design/tty/tty-tutor.md) and [architecture](design/tty/tty-arch.md)
- Diagnostics: [tutorial](design/diagnostics/diagnostics-tutor.md), [architecture](design/diagnostics/diagnostics-arch.md) and [x86_64 exception details](design/diagnostics/exception-handler.md)

## Process and historical records

Files under `process/` record a particular development stage or maintainer
workflow. Reviews, fix summaries and plans may mention removed paths or
completed limitations. Use [current limitations](current-limitations.md) for
current status and Git history when exact provenance matters.

- [Branch strategy](process/branch-strategy.md)
- [Coding style](process/coding-style.md)
- [Multi-architecture strategy](process/multi-arch-strategy.md)
- [ARM64 development setup](process/arm64-dev-setup.md)
- [ARM64 porting diary](process/arm64-port_zh.md)
- [ARM64 e1000 interrupt roadmap](process/arm64-e1000-interrupt-roadmap.md)
- [ARM64 code review (2026-07-17)](process/arm64-code-review-2026-07-17.md)
- [M3a EL0 fix summary](process/m3a-el0-fix-summary.md)
- [VMM review](process/vmm-review.md)
- [Resolved issues archive](process/known-issues.md)
- [Codex review](process/codex-review_zh.md) and [fix summary](process/codex-fixes_zh.md)
- [Dated discussions](process/discussions/2026-07-16-meaning-and-direction.md)
