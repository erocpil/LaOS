# LaOS Documentation

LaOS is a teaching kernel for x86_64 and ARM64. The shared documentation and
the book in this checkout follow the x86_64 implementation. The `arm64`
branch carries its architecture implementation and branch-specific narrative.

中文阅读入口：[book 中文目录](book/README-zh.md)。各文档使用 `-zh.md`
作为中文版本后缀，正文顶部提供中英文切换链接。命令、接口与日志标志保持原样。

## Start here

- [Getting started](getting-started.md) — dependencies, first build, tests and troubleshooting.
- [Test guide](testing-guide.md) — maintained test methods and detailed documentation.
- [Current limitations](current-limitations.md) — authoritative capability boundary.
- [Task configuration DSL](task-conf-dsl.md) — modules, users and selftests declared in `task.conf`.
- [Branch strategy](process/branch-strategy.md) — shared-first multi-architecture workflow.

## Read the kernel as a book

[Reading LaOS](book/README.md) follows one path from boot to user execution:

1. [Boot](book/01-boot.md)
2. [Memory](book/02-memory.md)
3. [Interrupts](book/03-interrupt.md)
4. [Scheduling](book/04-scheduler.md)
5. [User mode](book/05-user-mode.md)
6. [Devices](book/06-devices.md)
7. [Synchronization](book/07-synchronization.md)
8. [Porting](book/08-porting.md)
9. [Walkthrough](book/09-walkthrough.md)

中文章节：[启动](book/01-boot-zh.md)、[内存](book/02-memory-zh.md)、
[中断](book/03-interrupt-zh.md)、[调度](book/04-scheduler-zh.md)、
[用户态](book/05-user-mode-zh.md)、[设备](book/06-devices-zh.md)、
[同步](book/07-synchronization-zh.md)、[移植](book/08-porting-zh.md)、
[源码与测试串讲](book/09-walkthrough-zh.md)。

## Design and implementation notes

- Boot: [tutorial](design/boot/boot-tutor.md) and [architecture](design/boot/boot-arch.md)
- Memory: [tutorial](design/memory/memory-tutor.md) and [architecture](design/memory/memory-arch.md)
- Interrupts: [tutorial](design/interrupt/interrupt-tutor.md) and [architecture](design/interrupt/interrupt-arch.md)
- User mode and syscalls: [tutorial](design/user/user-tutor.md) and [architecture](design/user/user-arch.md)
- PCI: [tutorial](design/device/pci-tutor.md) and [architecture](design/device/pci-arch.md)
- e1000: [tutorial](design/device/e1000-tutor.md) and [architecture](design/device/e1000-arch.md)
- Storage: [tutorial](design/storage/storage-tutor.md) and [architecture](design/storage/storage-arch.md)
- Kernel modules: [tutorial](design/module/module-tutor.md) and [architecture](design/module/module-arch.md)
- Testing: [entry](testing-guide.md), [tutorial](design/testing/testing-tutor.md) and [architecture](design/testing/testing-arch.md)
- RCU: [tutorial](design/sync/rcu-tutor.md) and [architecture](design/sync/rcu-arch.md)
- Scheduler: [tutorial](design/scheduler/scheduler-tutor.md) and [architecture](design/scheduler/scheduler-arch.md)
- Mutex: [tutorial](design/sync/mutex-tutor.md) and [architecture](design/sync/mutex-design.md)
- TTY and monitor: [tutorial](design/tty/tty-tutor.md) and [architecture](design/tty/tty-arch.md)
- Diagnostics: [tutorial](design/diagnostics/diagnostics-tutor.md), [architecture](design/diagnostics/diagnostics-arch.md) and [x86_64 exception details](design/diagnostics/exception-handler.md)

## 中文设计文档

| 主题 | 教程 | 架构说明 |
| --- | --- | --- |
| 启动 | [启动教程](design/boot/boot-tutor-zh.md) | [启动架构](design/boot/boot-arch-zh.md) |
| 内存 | [内存教程](design/memory/memory-tutor-zh.md) | [内存架构](design/memory/memory-arch-zh.md) |
| 中断 | [中断教程](design/interrupt/interrupt-tutor-zh.md) | [中断架构](design/interrupt/interrupt-arch-zh.md) |
| 调度 | [调度教程](design/scheduler/scheduler-tutor-zh.md) | [调度架构](design/scheduler/scheduler-arch-zh.md) |
| 用户态与系统调用 | [用户态教程](design/user/user-tutor-zh.md) | [用户态架构](design/user/user-arch-zh.md) |
| PCI | [PCI 教程](design/device/pci-tutor-zh.md) | [PCI 架构](design/device/pci-arch-zh.md) |
| e1000 | [e1000 教程](design/device/e1000-tutor-zh.md) | [e1000 架构](design/device/e1000-arch-zh.md) |
| 互斥锁 | [互斥锁教程](design/sync/mutex-tutor-zh.md) | [互斥锁架构](design/sync/mutex-design-zh.md) |
| RCU | [RCU 教程](design/sync/rcu-tutor-zh.md) | [RCU 架构](design/sync/rcu-arch-zh.md) |

存储、模块、测试、TTY 和通用诊断文档目前提供英文版本，入口见上一节。
[x86_64 异常处理设计](design/diagnostics/exception-handler.md)已有中文正文。

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
