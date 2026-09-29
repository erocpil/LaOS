# Reading LaOS

[中文版](README-zh.md)

These chapters follow the current x86_64 implementation from boot to running
user tasks. They use the same chapter sequence as the ARM64 narrative, but
describe code available in this checkout. Architecture-specific implementation
on `arm64` remains branch-local.

The book explains how subsystems fit together. Documents under `design/`
provide focused tutorials and implementation contracts. Start with
[Getting started](../getting-started.md) for dependencies and use
[Current limitations](../current-limitations.md) when evaluating capabilities.

| Chapter | Question |
| --- | --- |
| [1. Boot](01-boot.md) | How does bootloader state become a running kernel? |
| [2. Memory](02-memory.md) | Who owns pages, mappings and virtual ranges? |
| [3. Interrupts](03-interrupt.md) | How can interrupted execution resume safely? |
| [4. Scheduling](04-scheduler.md) | Which eligible thread runs next? |
| [5. User mode](05-user-mode.md) | How does a user program enter and leave the kernel? |
| [6. Devices](06-devices.md) | How do discovery, DMA and storage fit together? |
| [7. Synchronization](07-synchronization.md) | Which ordering and lifetime guarantee is needed? |
| [8. Porting](08-porting.md) | Which contracts must another architecture implement? |
| [9. Walkthrough](09-walkthrough.md) | How do source and test evidence explain one execution? |

Commands run from the repository root. Exercises are either source-reading
tasks or use existing test targets; illustrative traces are not exact serial
output unless explicitly identified as a marker. The test target's actual
assertions determine what a pass establishes.

Return to the [documentation index](../index.md).
