# 启动架构

英文原文：[Boot architecture](boot-arch.md)。

## 适用范围

本文说明当前目录中的 x86_64 Limine 启动路径。ARM64 实现位于 `arm64` 分支，直接启动和 Limine 路径需要分别评估。源码阅读步骤见 [启动教程](boot-tutor-zh.md)，整体过程见 [第 1 章](../../book/01-boot-zh.md)。

## 源码分工

| 文件 | 职责 |
| --- | --- |
| `kernel/arch/x86_64/main.c` | Limine 请求、BSP/AP 入口及启动任务准备 |
| `kernel/boot_limine.c` | 将 Limine 响应转换为共享启动记录 |
| `kernel/boot_info.h` | 内存映射与模块交接格式 |
| `kernel/hhdm.h` | 直接映射地址转换 |
| `kernel/arch/x86_64/cpu.c` | 每 CPU 状态与上线协调 |
| `kernel/task.c` | 构造并释放已配置任务 |

入口为 x86_64 链接脚本指定的 `kmain()`。启动信息来自 `.limine_requests` 中静态请求的响应字段，没有单个启动信息参数传入 `kmain()`。

## 启动信息所有权

`check_bootloader()` 检查基础修订版本以及所需 framebuffer、HHDM 和内存映射响应。在调用 `boot_info_from_limine()` 前先初始化直接映射偏移。

适配层将内存映射条目和模块描述符复制到静态数组，当前上限分别为 128 和 64。模块内容、路径和命令字符串仍通过指针引用，不是引导程序内存的深复制。回收引导程序存储之前，须明确审查这些指针的生命周期。

CPU 数限制为 `MAX_CPUS`。逻辑 CPU 编号来自 Limine CPU 数组，不应与可能不连续的 LAPIC ID 混淆。

启动记录具有借用存储边界。复制后的数组条目由 LaOS 管理，而模块内容、路径和命令字符串仍由指针引用并由引导程序管理。未来若要回收启动内存，必须先完成深复制，或证明所有消费者均已停止使用这些引用。

## BSP 初始化顺序

以下分组保持 `kmain()` 中的实际顺序：

1. 初始化日志，检查启动响应，初始化 TTY 并启用 FPU。
2. 建立早期 GS、GDT/TSS 和 IDT，屏蔽传统 PIC。
3. 初始化 PMM、内核页表根、堆和模块分配器。
4. 映射 LAPIC 与 PCI 配置空间，初始化 LAPIC、I/O APIC 和 syscall MSR。
5. 运行同步 VMA、CPIO、LaFS 和块设备检查；在 PCI 枚举及真实 virtio-blk 初始化前清空模拟块设备注册表。
6. 发现 e1000 中断线并启动辅助 CPU。
7. 解析启动模块和任务，初始化主线程，参与上线屏障；注册、配置并启动自测试。
8. 加载嵌入式用户 ELF，释放配置任务，启用中断并进入监视器与主线程调度循环。

上述顺序表达初始化依赖，不保证每阶段均有事务式失败恢复。例如，缺少必需响应导致 panic；缺少 virtio 块设备则输出跳过消息。

该顺序可以划分为三个门槛：

```text
接受引导程序契约 -> 建立内核内存/CPU 状态
                  -> 初始化设备和任务
                  -> 启用中断并释放调度
```

在 IDT、控制器和每 CPU 状态准备好之前启用中断，可能将普通异步事件变成早期启动故障。在分配器、页表根或调度器状态准备好之前启动任务，也会产生类似的所有权问题。

## AP 初始化顺序

`start_smp()` 将 `secondary_cpu_init` 写入各辅助 CPU 的 Limine `goto_address`。AP 建立 GS，采用内核页表根，加载 GDT/TSS 和 IDT，初始化 LAPIC 及每 CPU 状态，配置 syscall MSR，随后参与 `wait_online()`，释放任务、启用中断并调度。

即使 IDT 表等对象共享，每 CPU 仍须加载对应描述符寄存器。MSR 配置同样只影响执行该操作的 CPU，BSP 启动成功不能证明 AP 已准备完毕。

AP 上线是本地契约：被 `wait_online()` 计数之前，AP 必须具备有效 GS、描述符表、中断控制器状态、页表根和调度器状态。共享 IDT 等对象不能免除每 CPU 寄存器和 MSR 设置。

## 两种用户程序构造路径

配置中的 type-3 任务通过 `create_elf_process()` 获取独立用户页表根。`kmain()` 另通过本地 `load_user_elf()` 从嵌入式 CPIO 加载演示 ELF，该线程将内核页表根记录到 `pml4_phys`。

分析日志与隔离性质时必须区分两条路径。多条用户消息不能单独证明全部来自独立地址空间。详细约束见 [用户态架构](../user/user-arch-zh.md)。

## 验证与限制

```sh
make test-x86_64
make test-x86_64-smp-tlb
```

普通目标检查运行标志及命名自测试结果，TLB 目标提供专项跨 CPU 转换依据。两者都不能证明通用固件兼容性或任意 CPU 拓扑支持。其他验证应按 [测试矩阵](../testing/testing-arch.md#coverage-matrix)选择。

启动证据应按阶段记录：接受启动响应、内存所有权、BSP 架构设置、AP 上线完成、设备发现、任务构造以及最终进入中断/调度。最终运行标志只能证明选定路径到达监视器，不能证明所有可选设备或用户任务均已成功完成。
