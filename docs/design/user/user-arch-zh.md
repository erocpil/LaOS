# 用户态架构

英文原文：[User-mode architecture](user-arch.md)。

## 适用范围

LaOS 运行小型静态链接用户 ELF，提供六个教学系统调用。当前 x86_64 支持独立用户页表根、线程内核栈续执行状态、可抢占 syscall 和惰性映射，不支持 fork、execve、waitpid、信号或 POSIX 文件描述符表。

## 源码分工

| 文件 | 职责 |
| --- | --- |
| `kernel/elf_loader.c` | 配置用户线程构造 |
| `kernel/elf.c` | ELF 段复制与初始用户栈 |
| `kernel/thread.c` | 线程生命周期与销毁 |
| `kernel/arch/x86_64/thread_arch.h` | 初始切换帧 |
| `kernel/arch/x86_64/syscall_entry.asm` | syscall 入口、切栈与返回 |
| `kernel/arch/x86_64/syscall.h` | 调用号与 trap frame 布局 |
| `kernel/arch/x86_64/syscall.c` | MSR、分发与首次用户转换 |
| `user/crt0.s`、`user/user.lib.c` | ELF 入口及 syscall 包装 |

## 构造与首次进入

配置用户任务调用 `create_elf_process()`，复制 ELF，初始化 TCB 和优先级，创建用户根，加载 `PT_LOAD`，准备参数栈及独立内核栈。任务层随后分配目标 CPU 并入队。

用户根共享内核高半区。数据通过物理页 HHDM 别名复制，权限来自 ELF 标志，段内 BSS 字节清零。用户栈包含 `argc`、用户地址的 `argv` 和字符串，不提供动态链接器及辅助向量等完整宿主进程环境。

架构入口准备 RIP、CS、RFLAGS、RSP 和 SS，通过 `iretq` 首次进入 ring 3。CRT 转换栈布局并调用 main。用户程序应显式调用 exit，当前 CRT 在 main 返回后执行 `hlt`。

`kernel/arch/x86_64/main.c` 另有本地嵌入式 ELF 示例，其 `load_user_elf()` 使用内核根，不是私有根加载器的另一实例。

## Syscall ABI

RAX 保存调用号，参数依次使用 RDI、RSI、RDX、R10、R8、R9，结果返回 RAX。当前调用最多三个参数。syscall 硬件覆盖 RCX 和 R11。

| 编号 | 接口 | 当前返回约定 |
| --- | --- | --- |
| 1 | `write(fd, buf, count)` | 请求长度或 -1；仅 fd 1、2 |
| 3 | `yield()` | 调度后返回 0 |
| 5 | `mmap(length, prot, flags)` | 虚拟地址或 0 |
| 6 | `munmap(addr, length)` | 0 或 -1，精确匹配 VMA |
| 35 | `msleep(msec)` | tick 休眠结果；零表示让出 CPU |
| 60 | `exit(status)` | 不返回 |

未知调用返回 -1。这是 LaOS ABI，不是 Linux 兼容层。映射限制见 [内存架构](../memory/memory-arch-zh.md)。

## 完整 syscall 帧

汇编切换 GS，在每 CPU 临时槽保存用户 RSP，再切换至当前线程内核栈顶，构造 18 槽、144 字节的 `struct trap_frame`：

| 字节偏移 | 按内存顺序排列的字段 |
| --- | --- |
| 0–32 | `rax`、`r12`、`r13`、`r14`、`r15` |
| 40–80 | `r9`、`r8`、`r10`、`rdx`、`rsi`、`rdi` |
| 88–96 | `rbx`、`rbp` |
| 104–136 | `rip`、`cs`、`rflags`、`rsp`、`ss` |

rip 保存 RCX 提供的返回地址，rflags 保存 R11 提供的标志。硬件已经覆盖 RCX/R11，因此没有指令前原值的独立快照。其他返回所需通用寄存器均有槽位，FPU 状态位于线程独立切换保存区，不在此帧内。

`syscall_handler()` 将结果写入 `regs->rax`。汇编恢复寄存器，从返回字段加载 RCX/R11，恢复用户 RSP、切回 GS 并执行 sysret。CS/SS 槽描述上下文，实际返回选择子由 MSR 约束提供，不从栈弹入段寄存器。

帧顺序由汇编和 C 手工约定。生成偏移覆盖每 CPU/线程访问和选择子常量，不覆盖全部 trap frame 字段。布局修改须同步更新两处并验证 C 调用边界栈对齐。

## 抢占与保存帧生命周期

SFMASK 在入口屏蔽中断。帧完整后 C 启用中断，定时器可在该线程内核栈挂起 syscall。每 CPU 用户 RSP 仅用于受保护入口与出口期间的临时存储，持久值位于帧的 rsp 槽。

出口 C 关闭中断并通过 schedule 处理待执行重调度，再返回汇编恢复路径。syscall 帧不是 IRQ 帧，不能复用 IRQ 调度与恢复约束。

## 进程模型与验证限制

完整保存帧只是复制执行状态的前提之一。`ret_from_fork` 是首次线程运行跳板。fork 仍需地址空间复制、父子返回值、所有权与失败清理及生命周期语义。

其他限制包括畸形 ELF 边界检查不完整、没有通用可恢复故障的用户复制 API，以及合成 sysret 目标验证不完整。write 检查区间和长度，但直接读取用户内存并按字符串分块打印，不是二进制安全的 POSIX write，也不能安全探测任意用户指针。

```sh
make test-x86_64
make test-x86_64-multiuser
```

第二目标统计至少三个完成消息，提供生命周期冒烟依据，但不检查全部保存寄存器哨兵值、退出回收或 fork。阅读方法见 [用户态教程](user-tutor-zh.md)。
