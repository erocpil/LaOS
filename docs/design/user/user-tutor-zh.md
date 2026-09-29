# 用户态教程

英文原文：[User-mode tutorial](user-tutor.md)。

本文分析配置用户 ELF 的加载、syscall 和返回。物理页与用户地址的区别见 [内存教程](../memory/memory-tutor-zh.md)。

## 1. 选择配置用户路径

`conf/task-x86_64-multiuser.conf` 的三个 type-3 记录将任务放置到 CPU 0。沿任务构造进入 `kernel/elf_loader.c` 的 `create_elf_process()`，不要改沿启动文件的嵌入式示例。

定位独立页表分配、PT_LOAD 循环、用户栈及初始架构帧。用户栈保存用户调用帧，内核栈保存服务或挂起该任务时的内核执行状态。

## 2. 首次用户指令

`arch_user_thread_entry_stub()` 构造五槽返回帧并进入 ring 3。`user/crt0.s` 的 `_start` 从栈读取 argc，argv 指向同一布局，并在调用 C 前对齐 RSP。

示例显式调用 exit(0)。当前 CRT 不将 main 返回转换为退出调用，应保留显式退出约定。

## 3. 分析一次 write

```text
用户 main -> write 包装 -> syscall
          -> swapgs、线程内核栈、trap_frame
          -> syscall_handler -> sys_write
          -> 保存 rax 结果、恢复寄存器 -> sysret -> 包装函数
```

比较包装输入、分发器及 `kernel/arch/x86_64/syscall.h` 保存字段。RCX/R11 由硬件用于返回状态，包装不能保证保留指令前的原值。

write 仅接受标准输出与标准错误，长度上限为 4096。它复制到内核栈缓冲并按字符串打印，对内嵌 NUL 和无效映射不提供宿主 C 库的保证。

## 4. 分析休眠调用

msleep 采用同一入口，将毫秒转换为 tick 后调用 schedule_timeout。其他线程运行期间，原线程内核栈保留未完成 syscall。恢复后处理程序完成并继续原用户指令流。

帧归线程所有，尽管入口通过每 CPU 状态查找线程；将其保存在共享每 CPU 栈上并不正确。

## 5. 检查现有目标

```sh
make test-x86_64-multiuser
```

检查加载、惰性页面写入与完成消息。单 CPU 配置便于观察重复调度。完成标志仍含历史名称 EL0，而 x86_64 实际特权级为 ring 3。

目标统计发生在退出调用之前的消息，不能据此判断页面泄漏或各次最终退出全部完成。

## 6. 练习：比较三类上下文

比较 syscall 帧、中断帧与 switch_to 帧，分别定位生成者、栈所有者和返回指令，再说明 fork 除复制 syscall 帧之外还需的页面所有权、子任务构造、两个返回值与失败回滚。

布局见 [架构说明](user-arch-zh.md)，整体叙述见 [第 5 章](../../book/05-user-mode-zh.md)。
