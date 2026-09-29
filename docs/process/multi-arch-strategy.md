# LaOS 多架构验证策略

> 最后更新：2026-07-25 | 当前状态：x86_64 ✅ | ARM64 ✅（Limine `task.conf`：动态模块 → EL0 → 模块恢复，双架构 virtio-blk + LaFS 真设备 CI 验证，跨架构 TLB 压测统一，测试 parity：x86_64 补齐 rollback/negative/sched-stress/multiuser，SYS_SLEEP 已确认实现，per-CPU 中断栈基础设施已分配） | RISC-V ❌（未开始）

---

## 一、架构矩阵

| 架构 | QEMU 机器 | 加速 | 启动时间 | 内存 | 内核产物 |
|---|---|---|---|---|---|
| **x86_64** | `q35` / `pc` | ✅ KVM | ~2 秒 | 2 GB | `bin-x86_64/kernel` / `build/LaOS.iso` |
| **ARM64** | `virt,gic-version=3` | ❌ TCG | 30-60 秒 | 512 MB | `bin-aarch64/kernel` |
| **RISC-V 64** | `virt` | ❌ TCG | 30-60 秒 | 512 MB | `bin-riscv64/kernel` |

---

## 二、三层验证模型

```
         ┌──────────────────┐
         │   RISC-V 64      │  ← 第三层：远期验证
         │  (TCG, 30-60s)   │     移植目标，当前以"能编译通过"为里程碑
         └────────┬─────────┘
                  │ 仅在 arch 相关改动时触发
         ┌────────┴─────────┐
         │   ARM64          │  ← 第二层：跨架构验证
         │  (TCG, 30-60s)   │     发现字节序/对齐/原子性/弱内存假设
         └────────┬─────────┘
                  │ 每次 git commit 前跑冒烟
         ┌────────┴─────────┐
         │   x86_64         │  ← 第一层：日常主力
         │  (KVM, ~2s)      │     TDD 循环、新功能、性能调优
         └──────────────────┘
                  ▲
            所有改动起点
```

### 2.1 第一层 — x86_64（日常主力）

- KVM 硬件加速，2 秒内完成启动+冒烟
- 所有新功能的**开发-TDD-调试**循环在这里
- `make -j && make test-x86_64` 是每次改动的硬门槛

### 2.2 第二层 — ARM64（提交前验证）

- TCG 纯软件模拟，30-60 秒
- **触发时机**（非每次改动）：
  - `git commit` 前跑一次冒烟
  - 改了 `thread.c`、`sched.c`、`elf_loader.c`、`pmm.c`、`vmm.c` 等核心共享模块后
  - 改了任何涉及指针大小、对齐、内存序的代码后
- **价值**：ARM64 是 64-bit LE 但对齐敏感 + 弱内存模型，能抓到 x86 的 TSO 掩盖的 bug

### 2.3 第三层 — RISC-V 64（远期里程碑）

- TCG 纯软件模拟，30-60 秒
- 当前阶段：**只要求能编译通过**
- 实际跑通是架构移植的 "hello world" 里程碑
- 至少等 ARM64 稳定跑通 M4+ 后再投入精力

---

## 三、Makefile 集成

```makefile
# 用法
make kernel ARCH=x86_64     # 构建 x86_64
make kernel ARCH=aarch64    # 构建 ARM64
make kernel ARCH=riscv64    # 构建 RISC-V

# 测试
make test-x86_64            # x86_64 冒烟（KVM，15s 超时）
make test-x86_64-lafs       # x86_64 virtio-blk + LaFS 真设备挂载
make test-x86_64-smp-tlb    # x86_64 TLB shootdown 压测（4 CPU，50 rounds）
make test-x86_64-rollback   # 模块加载事务 checkpoint/rollback
make test-x86_64-negative   # 缺失符号模块拒绝 + selftest init fail cancel
make test-x86_64-sched-stress # 调度器压力（4 worker × 200 rounds，4 CPU）
make test-x86_64-multiuser  # 多用户进程并发退出
make test-arm64             # ARM64 直启冒烟（成功标志出现后立即退出）
make test-arm64-limine      # Limine task.conf + 动态模块冒烟
make test-arm64-limine-negative # 拒绝缺失内核符号的模块后继续任务链
make test-task-conf-v1      # task.conf DSL v1 fixture 校验
make test-riscv64           # RISC-V 冒烟（TCG，120s 超时）
make test-all               # 三架构全跑
```

`test-arm64` 同时验收 EL0 write/exit 和网卡探测标志，由测试脚本
结束持续运行的 QEMU，不再固定等待 120 秒。`test-arm64-limine` 的成功
条件是用户任务进入就绪队列，且动态模块实际运行并打印已应用的参数。
它同样在强验收标志全部出现后主动结束 QEMU，不再依赖固定的运行等待时间。
ARM64 ISO 跟随 Limine 的 EFI boot-partition 布局，测试不需要 USB 键盘或
QEMU monitor 注入 Enter。
此外，`module_abi.mo` 对 AArch64 模块的 `ABS64`、`CALL26/JUMP26`、
`PREL32`、`ADRP+LO12`、`LDST8/32/64_LO12`、`.data` 和 `.bss`
同时执行 ELF 形态门禁与 QEMU 运行时验收。
`test-arm64-limine-negative` 使用独立配置加载引用不存在内核符号的
`module_bad.mo`，验证加载器拒绝故障模块后仍会调度 ABI 模块、普通模块和
EL0 任务。正常 ISO 不包含该故障模块。
模块加载器在分配 bump 区域前预检运行段重定位类型与外部符号，
避免可预知的失败路径泄漏模块 VA。

`kallsyms_all.c` 和 `git_version.c` 放在 `obj-<arch>/generated/<output>/`，
因此 x86_64、ARM64 及不同内核输出可以并行执行两阶段链接，不会互相
截断正在编译的生成源文件。

---

## 四、ARM64 开发环境

详见 [`arm64-dev-setup.md`](arm64-dev-setup.md)。

快速指令：

```bash
# 安装
sudo apt install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    libc6-dev-arm64-cross qemu-system-arm

# 构建 + 运行
make kernel ARCH=aarch64 -j$(nproc)
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a57 -m 512M \
    -kernel bin-aarch64/kernel -display none -serial stdio
```

---

## 五、ARM64 移植进度

| 里程碑 | 目标 | 状态 | 关键产出 |
|---|---|---|---|
| M0 | 最小串口输出 | ✅ | `kernel/arch/aarch64/` 骨架（25+ 文件） |
| M1 | 中断 + 时钟（GICv3 + Timer） | ✅ | entry.S 向量表、GIC 初始化、100Hz timer |
| M2a | 内存基础（PMM + VMM + MMU） | ✅ | identity mapping、block splitting、PMM memmap 通用化 |
| M2b | 上下文切换（switch.S） | ✅ | callee-saved 寄存器保存/恢复、x19 走私入口函数 |
| M2c | 抢占调度 + heap/vmm_map 修复 | ✅ | PTE_WRITABLE/PTE_IS_LEAF/PTE_AF 架构化、多线程交替 |
| M3a | EL0 入口 + SVC 系统调用 | ✅ | ERET → EL0 → SVC → handler 全链路 |
| M3b | EL0 中断处理 + 用户态调度 | ✅ | arch_enter_usermode、el0t_irq、用户态抢占 |
| M3c | ELF 加载器 + 用户程序 | ✅ | ELF→EL0→SVC write→exit 全链路 + `create_elf_process` + `debug.c` 异常处理器 |
| M3c+ | Limine UEFI 启动路径 | ✅ | EL1t 入口、SP_ELx trap 修复、嵌入式 ELF 阶段双路径曾通过 |
| M3d | Limine `task.conf` + 动态模块 | ✅ | 显式加载配置/ELF/`.mo`，TTBR1 模块 VA，AArch64 重定位，模块参数生效 |
|| M3d+ | ARM64 多任务连续调度 | ✅ | 模块 timeout → EL0 write/exit → 模块恢复；私有 TTBR0 MMIO 与清理修复 |
|| M3e | ARM64 Limine SMP bring-up + AP task dispatch | ✅ | 2 CPU/4 CPU 自动验证通过：AP parked/GIC/online + AP `task.conf` 模块 probe；TLB shootdown remap 可见性验证 |
|| P2 | 大页释放 + 线程帧抽象 | ✅ | `vmm_destroy_level` 大页释放修复、`thread_setup_user_frame` 去重 |
|| P3 | ARM64 僵尸文件清理 | ✅ | 删除 `acpi.h`、`ist.h`（零引用桩） |
|| P4 | 设备抽象 + 跨架构测试 | ✅ | virtio-blk/pci 双架构驱动、block_device 抽象、TLB 测试统一、双架构 LaFS CI 真设备验证 |
|
| P4 | 文档更新 | ✅ | 本文 + `arm64-port_zh.md` + `known-issues.md` 等 |

详细设计见 [`arm64-port_zh.md`](arm64-port_zh.md)。

`task.conf` 已规范为 LaOS 的 boot-time orchestration DSL。v1 兼容语法、
已支持的 `@test smp_tlb_remap` 和后续 directive 草案见
[`task-conf-dsl.md`](../task-conf-dsl.md)。

---

## 六、已发现问题

完整审查报告：[`arm64-code-review-2026-07-17.md`](arm64-code-review-2026-07-17.md)

| 级别 | 原始数量 | 已修复 | 剩余 | 最紧迫未修复项 |
|---|---|---|---|---|
| P0 | 2 | 2（AttrIndx 丢失、1GB page panic） | 0 | — |
| P1 | 5 | 4（硬编码页表遍历→vmm_map、EL0 timer hang→el0t_irq、arch_enter_usermode UB→实现、`lapic_eoi` 死代码→panic trap） | 1 | L3 条目覆盖物理页泄漏 |
| P2 | 6 | 1（注释过时部分更新） | 5 | 裸 hex 指令、UART 硬编码等 |

### ARM64 SMP 当前限制

- `bash run.sh smp` 默认 2 CPU 已作为自动验证入口，覆盖 AP parking、GIC CPU init、online 计数，以及 CPU1 从 Limine `task.conf` 启动 `module_smp_probe.mo`。
- `SMP_CPUS=4 bash run.sh smp` 已作为扩展验证通过，覆盖 3 个 AP 从 `task.conf` 分别启动 `smp1/smp2/smp3`。
- `bash run.sh smp-tlb` 默认 2 CPU 已作为跨 CPU TLB shootdown 入口，覆盖 BSP 广播 `IPI_VECTOR_TLB` 后 AP SGI ack，以及 `@test smp_tlb_remap rounds=4` 驱动的多轮 remap 可见性。
- `SMP_CPUS=4 bash run.sh smp-tlb` 已作为扩展验证通过，覆盖 3 个 AP 的 SGI TLB ack 和多轮 remap 可见性。
- AP task probe 目前是 IRQ-off smoke module：AP 侧只做原子标记，BSP timer 串行输出 `[smp-probe] cpu=N`，避免在 AP 释放阶段直接走尚未完全 SMP 化的 console/framebuffer 路径。
- 跨 CPU TLB 已覆盖 DSL 驱动的多轮 remap 可见性；还没有做 unmap 压力、AP 侧普通线程并发触发 VMM 更新。模块 sleep/exit/preemption 暂不作为 M3e 完成条件，后续应继续补 AP 侧可抢占调度压力。

### x86_64 TLB shootdown 审计

- x86_64 的 `vmm_remap()`、`vmm_unmap()`、`vmm_map_global()` 与 ARM64 共享调用点，都会调用 `ipi_broadcast(IPI_VECTOR_TLB)`。
- x86_64 IPI handler 在 `kernel/arch/x86_64/idt.c` 中对 `IPI_VECTOR_TLB` 执行 `arch_tlb_flush_all()`，实现方向合理。
- 验证覆盖：`make test-x86_64-smp-tlb` 已接入 CI，使用公共 `test_tlb.mo` 与 `conf/task-x86_64-tlb.conf`，通过 `@test smp_tlb_remap rounds=50` 驱动多轮 remap 可见性压测，覆盖 4 CPU。

### ARM64 模块加载器当前验证

- `bash run.sh rollback` 覆盖 ET_REL 加载事务失败后的 `module_alloc` checkpoint/rollback：故意加载无入口模块 `module_no_entry.mo`，要求出现 `[ module] module_alloc rollback:`，随后正常模块和 EL0 任务链继续通过。
- 该回滚只保证当前串行化加载事务的尾部分配回收；`module_free()` 仍为 no-op，真正的模块卸载和脱离 `task_lock` 的任意并发分配仍未实现。

---

## 七、架构隔离设计

LaOS 的架构隔离遵循 **kernel/ 零 `#ifdef`** 原则：

```
kernel/
├── arch/
│   ├── x86_64/          ← 完全独立，接口签名对称
│   ├── aarch64/         ← 完全独立，接口签名对称
│   └── riscv64/         ← 待创建
├── vmm.c                ← 共享（通过 arch_dispatch.h 引用架构头文件）
├── sched.c
├── thread.c
└── ...
```

每个 arch 目录提供同名头文件（如 `cpu.h`、`vmm_arch.h`、`arch_irq.h`），共享代码通过 `arch_dispatch.h` 间接引用。
