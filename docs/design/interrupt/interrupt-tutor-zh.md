# 中断教程

英文原文：[Interrupt tutorial](interrupt-tutor.md)。

中断挂起正在执行的指令流。C 代码运行之前，入口须保存足够状态以正确恢复。IDT 与每 CPU 设置的前置内容见 [启动教程](../boot/boot-tutor-zh.md)。

## 1. 分析定时器 tick

阅读 `kernel/arch/x86_64/idt_stubs.S` 的定时器 IRQ 桩，再沿 `common_stub`、`idt_handler()` 和 `irq_handler()` 检查调用。向量 32 到达 `kernel/timer.c` 的 `timer_handler()`。

```text
定时器 -> IDT 桩 -> 保存中断帧 -> C 处理
       -> 检查重调度 -> 恢复 -> iretq
```

tick 可以请求切换，但请求与实际切换是不同事件。公共返回按上下文决定是否允许调度。

## 2. 核对汇编与 C 布局

比较 `PUSH_ALL` 与 `kernel/arch/x86_64/idt.h` 的 `struct interrupt_frame`。压栈顺序与最终内存顺序相反。再比较有错误码和无错误码的异常桩，两者必须向 C 提供一致的向量与错误码布局。

定位保存 CS 的检查，说明何时需要改变 GS。syscall 期间发生的内核中断已经使用内核 GS，即使此前由用户程序进入内核。

## 3. 保存帧附属于线程栈

定时器抢占 syscall 并切换到另一线程时，原线程仍需保留 syscall 帧、C 调用帧与中断帧。恢复后先返回 syscall，再由 syscall 返回用户态。

致命异常专用栈用于正常栈不可信时的诊断，不表示每个 IRQ 均使用独立每 CPU 栈。

## 4. 异常与外部 IRQ

缺页异常说明指令访存问题，内核可能修复映射并重试。设备 IRQ 说明设备工作，可能需要清除设备原因。只有 APIC 投递中断需要对应控制器 EOI。

向共享返回路径增加确认之前，应分别检查 `idt_handler()` 中的两条路径，避免无条件 EOI 混淆职责。

## 5. 验证跨 CPU 投递

```sh
make test-x86_64
make test-x86_64-smp-tlb
```

`ipi_delivery`、`remote_enqueue` 和 `smp_tlb_remap` 分别提供中断到达、远程任务执行及转换变化可见性的依据，应检查各自结果。

异常注入配置及预期停止行为见 [诊断教程](../diagnostics/diagnostics-tutor.md)。

分析设备 IRQ 时，应分别记录四项观察：保存帧布局、设备原因清除、控制器 EOI 和重调度请求。不能用一条日志同时证明四项均正确。

## 6. 练习：分析嵌套返回

写出用户进入 write、定时器打断处理程序、另一线程运行、原线程恢复的返回顺序，标注 `iretq` 与 `sysret` 及每个保存帧的所有者。对照 [第 3 章](../../book/03-interrupt-zh.md)检查。

## 7. 练习：区分确认与进展

选择 e1000 路径，说明 IRQ 计数增加而 RX 处理仍然停滞的可能原因。定位谁清除 `ICR`、哪条路径确认控制器，以及哪个工作线程最终消费描述符。
