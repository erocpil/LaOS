# LaOS 已知 Bug / 待修复点

> **归档说明（2026-07-25）**
>
> 本文同时保存已修复问题和阶段性审查记录，不再作为当前能力边界的
> 唯一来源。当前限制请以 [current-limitations.md](../current-limitations.md)
> 为准；本文中的状态描述应结合所在段落日期和 Git 历史阅读。

按"发现日期 + 优先级"列出。不改即记，等具体优化任务把它们带上。

---

## 待改进（P1）：x86_64 SYSRET 返回状态校验

2026-11-04：`syscall_entry.asm` 已在入口保存 RCX 提供的返回 RIP，但出口仍直接
恢复用户 RSP、执行 `swapgs` 和 `SYSRET`，尚未检查最终 RIP。Intel CPU 上，
非规范 RCX 可使 `SYSRET` 在 Ring 0 触发 #GP，此时用户栈和用户 GS 状态已经
恢复，异常入口可能无法安全保存现场。

改进应在内核栈和内核 GS 仍有效时检查最终 RIP 的规范地址编码及用户地址范围，
并核对返回选择子、RFLAGS 等 SYSRET 前提。非法状态应终止任务或走明确的错误
处理；合法但不适用 SYSRET 的状态应走具备返回故障处理的 IRETQ 路径。不能仅
替换指令而忽略非法返回帧可能引发的异常。

入口可增加早期检查；入口/出口 RIP 比较只用于调试，不能代替最终状态检查。
未来信号、调试或系统调用重启可能合法修改 RIP。验收应覆盖合法返回、非规范
地址、规范但越出用户范围的地址，以及拒绝路径在内核栈和内核 GS 下执行。
当前 QEMU 正常系统调用通过不构成这些边界已受保护的证据。详见
[用户态架构](../design/user/user-arch-zh.md)。

## 待改进：PMM 输入校验、页面所有权与接口文档

2026-11-04：审查 `kernel/pmm.c` 和 `kernel/pmm.h` 后记录。以下项目尚未修复；
分配器当前仍以单个位图和全局 IRQ 安全锁管理物理页。

### P1：限制 `pmm_free()` 和 `pmm_free_pages()` 可释放的页面

当前位图只区分空闲与占用。初始化时被内核、位图及其他保留区间占用的页面，
与分配给调用者的页面没有不同标记；运行期释放接口因此无法确认页面是否由 PMM
分配。若调用方误传保留页的地址，释放可能使其随后被重新分配。应明确页面所有权
约定，并让运行期校验可以区分可分配页与永久保留页；不要仅凭位图中的“占用”状态
推断页面可释放。

### P2：使多页释放校验完整且避免部分修改

`pmm_free_pages()` 目前没有检查起始地址页对齐；`start + count` 的边界检查也可能
发生整数溢出。它在逐页清除位图时才发现重复释放，因此错误请求可能先修改前面的
页面，再触发 panic。应在持锁期间先验证地址、范围及整段页面状态，所有检查通过后
再统一更新位图和统计信息。所有加法应使用不会溢出的边界表达式。

### P2：规范并校验启动内存映射边界

初始化逻辑对映射项的页对齐和长度边界有隐含假设：统计使用向下取整的页数，释放
则按页步长遍历原始长度；基址未对齐或长度含部分页时，两者可能不一致。计算
`base + length`、向上舍入和位图大小时也未统一检查溢出。应先定义引导器输入的
对齐契约，再对每项进行溢出检查和一致的页区间规范化；位图放置还须确认所选可用
区间确实容纳完整位图，且不会覆盖仍需保留的内容。

### P2：修正 PMM 头文件与实现不一致的契约

`pmm.h` 对初始化输入、运行期锁、位图诊断函数和多页释放错误行为的描述与当前
实现不完全一致。例如，位图转储要求调用者保证 CPU 静止，而非自行加锁；多页重复
释放会 panic，且当前实现可能在 panic 前部分修改状态。应按实际实现逐项校正文档，
并明确各接口的地址类型、对齐、所有权、失败行为及锁约束。删除失效的路径引用、
历史性能数字和未实现的初始化阶段说明。

### P3：改善分配与诊断路径的可扩展性

连续页分配使用全局锁保护全位图首次适配扫描。页面数量增加或并发分配变多时，扫描
时间和锁持有时间可能上升。应先测量典型及高占用场景，再决定是否引入搜索提示、
分区或其他结构；不能仅以 `freed` 总数判断连续请求一定能满足，因为空闲页可能
分散。位图转储和页统计等诊断输出也应明确其快照一致性与调用约束，避免把非原子
观测描述为一致快照。

## 已修复: x86_64 AP 早期共享 BSP bootstrap GS context

2026-10-28：BSP 和 AP 通过逻辑 CPU 编号选择各自的静态早期上下文，安装 GS 前
初始化 `self` 和 `id`；重复的早期初始化保留已有状态。BSP 在入口关闭 IRQ 后立即
建立早期 GS，并在启动任一 AP 前安装正式上下文、完成共享 GDT 分配。AP 也先建立
独立早期 GS，再执行内存和处理器初始化。正式上下文的基础字段初始化完成后才安装
GS，切换前检查早期抢占计数为零。`cpu_test()` 不再修改 GS 基址。

## 已修复: x86_64 `preempt_enable()` 的无效检查与调用约束

2026-10-28：删除 GS 段选择子读取及其空分支，同时移除无其他调用者的 `get_gs()`。
`preempt_disable()` 和 `preempt_enable()` 明确要求调用前已安装有效的本地 GS
context，并严格配对。若 `%gs:16` 返回空 context，立即报告内核错误，不再静默返回；
计数下溢检查继续保留。`cpu_get_ctx()` 本身仍依赖 GS 基址有效，事后空指针检查无法
保护无效 GS 访问。`PREEMPT=0` 时两个接口直接编译为空操作，不访问 GS。

---

## Resolved: 模块加载事务失败回滚

ET_REL 加载器已增加 `module_alloc` checkpoint/rollback。分配后才发现的
入口缺失、重定位失败、线程创建失败等路径会回收本次加载事务的尾部
模块 VA、物理页和页表映射。

验证入口：

```sh
bash run.sh rollback
```

剩余边界：`module_free()` 目前仍为 no-op；真正 unload、引用计数，以及不依赖
`task_lock` 的任意并发模块分配仍未实现。

---

## Resolved: ARM64 跨 CPU TLB shootdown remap 验证

ARM64 Limine SMP 路径已增加跨 CPU TLB remap 可见性验证。BSP 在 AP online
后先广播 `IPI_VECTOR_TLB` 验证 SGI ack，再由 `@test smp_tlb_remap rounds=N`
把同一测试 VA 在 page A/page B 间多轮 remap；AP GIC SGI handler 执行
`arch_tlb_flush_all()` 后读取该 VA，必须看到每轮新值。

验证入口：

```sh
bash run.sh smp-tlb
SMP_CPUS=4 bash run.sh smp-tlb
```

剩余边界：该入口验证多轮 remap 可见性，不等价于 remap/unmap 混合压力或
普通线程并发页表更新压力。

---

## 已修复(归档)

### x86_64 virtio-pci DMA 地址 + 特性协商（P0/P1）
- **DMA 地址错误**：`virt_to_phys()` 假设 HHDM 映射，但 kmalloc 内存不在 HHDM → 改用 `pmm_alloc` + `phys_to_virt`，virtqueue 所有结构共享同一物理连续页
- **特性协商缺失**：未写 VIRTIO_F_VERSION_1 → 协商 VERSION_1，写 FEATURES_OK 后读回验证设备接受
- **轮询超时静默**：`virtio_pci_blk_poll()` 超时无失败信号 → 提交前 status byte 设 sentinel `0xFF`，超时返回 -1
- **修复日期**：2026-07-24

### 测试 parity：x86_64 补齐 rollback/negative/sched-stress/multiuser
- `test_sched_stress.c` 提升为双架构模块，Makefile + CI 已覆盖。
- 新增 `conf/task-x86_64-{rollback,negative,stress,multiuser}.conf`，`test-all` 全链路通过。
- `@module_missing` 已纳入 `check_task_conf_v1.sh` 校验白名单。
- **修复日期**：2026-07-24

### x86_64 SYS_SLEEP 误判澄清 + 中断栈基础设施
- **SYS_SLEEP (syscall 35)**：此前功能对比误判为"x86_64 缺失"。经审计确认 `sys_msleep()` → `schedule_timeout()` 早在 x86_64 实现，`user/lib.c` 已封装 `msleep()`，`user/main.c` 实际使用中。无需任何代码改动。
- **per-CPU 中断栈**：`cpu_context` 新增 `int_stack` / `int_stack_base` / `saved_rsp`，`per_cpu_init()` 为 BSP + AP 分配 8KB 中断栈，`asm_offsets` 自动生成汇编常量。实际 switch 因与 `switch_to` 交互复杂而延后——x86_64 调度后新线程 RSP 回自己的内核栈，`POP_ALL`+`iretq` 假设中断帧仍在当前栈顶；需要 per-thread `saved_rsp` 跟踪才能处理线程切换场景。
- **修复日期**：2026-07-25

### x86_64 virtio-pci 硬 Bug（P0）
- **desc_head 链错乱**：`submit_virtqueue` 每次重新扫描空闲描述符，`desc_head` 不固定 → 固定为 0
- **notify multiplier 截断**：struct cast 读取 PCI config space 多字节值 → 直接从 ptr+16 读 4 字节
- **test-x86_64-lafs 假阳性**：stub block device 覆盖真实设备 → `block_device_reset()` + 真实 virtio 挂载验证
- **修复日期**：2026-07-23

### x86_64 TLB shootdown remap 自动压测
- **修复**：test_tlb.c 与 test_tlb_x86.c 合并为公共双架构模块，
  `make test-x86_64-smp-tlb` 接入 `X86_64_QEMU_SMP` 可配 SMP，
  `X86_64_TASK_CONF` 可覆写 task.conf。ARM64 已有 `bash run.sh smp-tlb`
  和 `make test-arm64-limine-smp-tlb`。
- **修复日期**：2026-07-23

### sched.c: `check_need_schedule` 在 preempt_count ！= 0 时不阻断调度
- **位置**:`kernel/sched.c:14`
- **现象**:`return 0；` 被注释掉，函数继续返回 1 -> IRQ 路径调 `__schedule_irq` -> preempt_count ！= 0 触发 `Scheduling while atomic` panic
- **修复**:commit `af59907`，取消注释
- **修复日期**:2026-06-26

### sched.c: `__schedule_irq()` 传错 preemptive 标志
- **位置**:`kernel/sched.c:311`
- **现象**:IRQ 返回路径调度本应是抢占式(preemptive=true)，但传了 false，导致 `__schedule` 走 panic 分支而不是早退分支
- **修复**:commit `af59907`，改为 `__schedule(true)`
- **修复日期**:2026-06-26

### stats.c: `__stats_cpu` 死代码 + monitor.c 双重 fb_clear_screen
- **位置**:`kernel/stats.c:250-301`,`kernel/monitor.c:26`
- **现象**:旧版 `__stats_cpu` 用 `cpu_get_ctx()` 而非 `g_cpu_contexts[i]`，统计全错；monitor 每帧调用 `fb_clear_screen` 两次(一次在 monitor.c，一次在 render_tty9_monitor 内部)，造成可见闪烁
- **修复**:commit `3e78ebd` / `6e00680`
- **修复日期**:2026-06-25

### kprintf 串口丢失 (P2)
- **位置**:`kernel/printf.c:621-660`
- **现象**:SMP 后若 `tty_ready()` 返回 false，早期实现直接 return，连 `serial_puts` 也不调用，导致 monitor entry 等早期 L 静默丢失
- **修复**:将 `serial_puts(buf)` 提至 `tty_ready()` 检查之外独立执行；fb 输出受 TTY 过滤，串口始终输出（调试通道）
- **修复日期**:2026-07

### monitor.c "Momitor" 拼写 (P3)
- **位置**:`kernel/monitor.c:20` → `kernel/monitor.c:110`
- **现象**:L("Momitor Started") → L_TAG(..., "Monitor started.")
- **修复**:拼写修正
- **修复日期**:2026-07

### ARM64 lapic_eoi 死代码 (P2)
- **位置**:`kernel/arch/aarch64/lapic.h:17-31`
- **现象**:旧实现重读 IAR → 错误 EOI；若被调用会去激活另一个中断
- **修复**:改为 `panic()` 桩，注明需使用 `EOI(frame->int_no)`
- **修复日期**:2026-07

### ARM64 test_el0.S UART 硬编码 (P3)
- **位置**:`kernel/arch/aarch64/tests/test_el0.S`
- **现象**:UART MMIO 基址原硬编码 `0x09000000`
- **修复**:改为 `UART0_BASE` 汇编常量引用（`asm_defs.h`）
- **修复日期**:2026-07

---

## ARM64 移植已知问题

按优先级排列（2026-07-18 状态）。

### 已修复: ARM64 后继任务调度停滞

**涉及位置**：ARM64 用户页表、Limine `kernel_pml4`、用户地址空间清理和串口退出日志

Limine `task.conf` 同时创建动态模块线程和 EL0 用户线程后，曾表现为模块 timeout 后
无法连续运行 EL0，或 EL0 退出后模块不恢复。寄存器采样最终确认是 EL1 在私有 TTBR0
下访问未映射的低地址 PL011 引发 Data Abort，并非 runqueue/`switch_to` 选择错误。

**修复（2026-07-18）**：为用户 TTBR0 和 Limine `kernel_pml4` 补齐 supervisor-only
PL011 映射；用 `arch_user_vmm_init/destroy` 封装平台 MMIO 映射的生命周期，
销毁用户地址空间前解除非 PMM 所有的叶映射；退出与模块验收日志改走无锁串口。
自动测试现要求模块启动、EL0 write/exit、模块 timeout 后恢复三个标志同时出现，且
serial log 中不得出现 `PANIC`。

### 已解决: ARM64 Limine AP task.conf smoke module

**涉及位置**：`kernel/arch/aarch64/limine_main.c`、`kernel/task.c`、模块加载器

当前 AP bring-up 已验证到：所有 Limine AP 完成 park、per-CPU GIC init、
local timer init、`online` 计数、scheduler 启动，并能从 `task.conf`
按 CPU 绑定启动 `module_smp_probe.mo`。`bash run.sh smp` 和
`SMP_CPUS=4 bash run.sh smp` 均通过。

修复要点：

- ARM64 `ret_from_fork` 把 x19 中的入口函数显式传入 x1，避免 C 函数体内
  读取 x19 的未定义假设。
- task.conf 模块入口用 `!is_user && entry_argv != NULL` 判定，避免无参数模块
  误走 `func(data)` 旧路径，也避免用户线程被当成内核模块
  `laos_module_entry(argc, argv)`。
- AP probe 模块只做 IRQ-off 原子标记，由 BSP timer 串行输出 `[smp-probe]`。

### 已解决: 用户 ELF 嵌入内核镜像（70KB+）

**涉及位置**：原 `user_elf_embed.h`（通过 `xxd -i` 生成），已移除。

**修复（P4-5）**：用户程序改为 CPIO initrd 动态加载。`script/mkcpio.py` 构建 CPIO
档案，`script/embed_bin.py` 生成 `initrd_embed.h`（build 时产生，gitignored）。
以上为历史迁移记录。当前 x86_64 已移除嵌入式用户启动路径；`make iso-base`
通过 Limine 交付外置基础归档，配置任务统一使用私有根加载器。ARM64 尚未同步
此轮改动。

### 设计限制（非 Bug）

| 限制 | 说明 | 计划 |
|------|------|------|
| `write()` 忽略 fd | 仅输出到 PL011 UART / x86_64 串口 | shell 合并后统一 |
| LaFS 只读 (x86_64 + ARM64) | virtio-blk/pci + LaFS 已可用，两架构均有块设备驱动，无写入路径 | M4+ |
|| 无 sbrk | 用户堆不可动态扩展 | M4+ |
|| mmap/munmap 已实现 | VMA + 按需分页（两架构均已可用） | — |
| EL0 timer 在无调度场景 hang | `el0t_irq` 需调度器上下文 | M4 SMP 后修复 |
