# ARM64 移植代码审查报告

**审查日期**：2026-07-17
**更新日期**：2026-07-14 — 标记已修复项。P0 全修，P1 3/5 修，详细状态见 [`multi-arch-strategy.md`](multi-arch-strategy.md#六已发现问题)
**范围**：`kernel/arch/aarch64/` 全部 33 文件 + `kernel/vmm.c` (block splitting) + `kernel/arch/x86_64/vmm_arch.h`
**阶段**：M3a（EL0 用户态入口刚打通）

---

## 架构概览

ARM64 移植采用"共享内核 + 架构桩"模式，与 x86_64 共用调度器/内存管理/模块系统。差异通过 `arch/` 下同名头文件和 `.c/.S` 隔离。

模块状态一览：

| 模块 | 文件 | 状态 |
|---|---|---|
| 启动 / EL 降级 | `startup.S` | ✅ EL3→EL2→EL1 链正确 |
| 异常向量表 | `entry.S` | ✅ 分支跳板模式，16 slot 全部正确偏移 |
| 上下文切换 | `switch.S` | ✅ callee-saved 保存/恢复 |
| 中断控制器 | `gic.c` / `gic.h` | ✅ GICv3 初始化正确 |
| IRQ 控制 | `arch_irq.h` | ✅ DAIF 操作，safe_halt 正确 |
| 串口 | `serial_arch.h` | ✅ PL011 初始化 + TX 等待 |
| CPU 上下文 | `cpu.h` / `cpu.c` | ✅ TPIDR_EL1 per-CPU |
| MMU/页表 | `vmm_arch.h` | ✅ 4-level 4KB granule |
| TLB | `arch_tlb.h` | ✅ tlbi + DSB+ISB |
| 内存屏障 | `arch_barrier.h` | ✅ DMB/DSB/ISB |
| 系统调用 | `syscall.c/h` | ✅ SVC 分发骨架 |
| EL0 入口 | `el0_test.S` | ✅ ERET 到 EL0 |
| 桩文件 | `gdt.h/acpi.h/ist.h/ipi.*` | ✅ 编译桩 |

---

## P0 — 崩溃/内存损坏

### P0-1：Block splitting 丢失内存类型 (AttrIndx) ✅ 已修复

**文件**：`kernel/vmm.c:343-347`
**严重度**：P0（静默数据损坏 — DRAM 被当作 device memory）
**修复日期**：2026-07-17

2MB block 拆分时，属性掩码只保留了少数几位：

```c
(block_attrs & (PTE_USER | PTE_WRITABLE | PTE_NX
               | PTE_WRITE_THROUGH | PTE_CACHE_DISABLE))
```

**遗漏的关键位**：AttrIndx (bits [4:2])，控制内存类型。

| 位域 | 原始 block | 拆分前 L3 | 修复后 L3 |
|---|---|---|---|
| AttrIndx | 1 (normal WBWA) | 0 (device-nGnRnE) | 1 (normal WBWA) |

MAIR_EL1: Attr0=device, Attr1=normal WBWA。修复前拆分后所有 L3 条目指向 Attr0 → **设备内存**。

**修复方案**：`l3[i] = (block_base + i*0x1000) | block_attrs | VMM_LEAF_EXTRA_FLAGS` — 保留原 block **全部**属性位（bits[20:0]），确保 AttrIndx、SH、AP 等完整继承，而非选择性掩码。

**验证**：`make ARCH=aarch64 -j && make ARCH=x86_64 -j` 通过，QEMU ARM64 EL0 入口测试正常（SVC 分发 → heap 操作 → 全链路验证通过）。


### P0-2：1GB huge page 拆分被 panic 替代（x86_64 回归） ✅ 已修复

**文件**：`kernel/vmm.c:311-312`
**严重度**：P0（x86_64 启动 panic）
**修复日期**：2026-07-17

原 x86_64 代码有 1GB page → 512×2MB block 拆分，被改为 `panic(...)`。

**修复方案**：恢复 1GB→512×2MB 拆分逻辑，用 `PTE_TABLE_USER_MASK(flags)` 替代旧的 `flags & PTE_USER`。ARM64 的 `PTE_TABLE_USER_MASK` 返回 0（表描述符无 AP 位），x86_64 返回 bit2 正常继承。架构无关，无需 `#ifdef`。

**验证**：x86_64 编译通过（未实际测试 x86_64 1GB page 启动，但逻辑与修复前一致）。

---

## P1 — 逻辑错误 / 潜在问题

### P1-1：main.c 硬编码页表遍历 ✅ 已修复（2026-07-17）

**文件**：`kernel/arch/aarch64/main.c:191-196`

原硬编码 `l0[0]`/`l1[1]`/`l2[0]` 遍历已改为 `vmm_map` 操作。修复在 M3b 中完成。


### P1-2：EL0 定时器中断静默 hang ✅ 已修复（2026-07-17）

**文件**：`entry.S` (`VENTRY_UNHANDLED el0t_irq`)，`el0_test.S`

原 `VENTRY_UNHANDLED` → `b .` 静默挂死已通过实现 `el0t_irq` handler（EL0 上下文保存/恢复）修复。在 M3b 中完成。

### P1-3：arch_enter_usermode 是 UB 桩 ✅ 已修复（2026-07-17）

**文件**：`kernel/arch/aarch64/entry_arch.h`

原 `__builtin_unreachable()` 桩已替换为完整的 SPSR+ELR+SP_EL0 设置 + `eret`，进入用户态前清零 x0（argc=0）。在 M3b 中完成。


### P1-4：lapic_eoi 实现误导

**文件**：`kernel/arch/aarch64/lapic.h:17-31`

`lapic_eoi()` 实现为重读 IAR → EOI，但 ARM64 实际 EOI 路径是 `idt.h` 宏（用已保存的 `frame->int_no`）。`lapic_eoi` 是死代码，若未来被调用会重读一个**不同的** IAR 值并对其发送 EOI。


### P1-5：L3 条目覆盖导致旧物理页泄漏

**文件**：`kernel/arch/aarch64/main.c:196`

Block splitting 为 VA `0x40101000` 创建了指向 PA `0x40101000` 的 L3 条目。后续 `l3[257] = phys | ...` 覆盖为 pmm_alloc 分配的新帧，原始 PA `0x40101000` 的映射丢失 → 物理页泄漏。当前只分配一次影响小。

---

## P2 — 代码风格/清理

| ID | 文件 | 问题 | 建议 |
|---|---|---|---|
| P2-1 | `entry.S` | 未处理异常静默 `b .` | 写入 '!' 到 UART |
| P2-2 | `el0_test.S:17` | 硬编码 UART 地址 `0x09000000` | 后续用宏或 gen_offsets |
| P2-3 | `cpu.c:6` | 注释"M0 阶段"已过时 | 更新为 M3a |
| P2-4 | `entry_arch.h:5` | 同上 | 更新注释 |
| P2-5 | `main.c:38` | 裸 hex 指令 `0xD4000041` | `.inst` 或内联汇编宏 |
| P2-6 | `entry.S` | 无 "el0t_irq 由 timer 触发后 hang" 的文档 | 添加注释 |

---

## 跨架构一致性

| 接口 | ARM64 | x86_64 | 判定 |
|---|---|---|---|
| `PTE_TABLE_USER_MASK(f)` | `0` | `(f) & PTE_USER` | ✅ 合理（ARM64 表描述符无 AP 位） |
| `VMM_LEAF_EXTRA_FLAGS` | `PTE_AF | ARM64_DESC_TABLE` | `0` | ✅ 架构差异 |
| `switch_to` 走私 | x19 (callee-saved) | R15 | ✅ 对称 |
| per-CPU | TPIDR_EL1 | GS 段 | ✅ 等价 |
| `arch_enter_usermode` | UB 桩 | sysret 实现 | ❌ 见 P1-3 |

---

## 安全边界

- ✅ EL 降级链：SCR_EL3.NS=1, HCR_EL2.RW=1
- ✅ MMU 初始化：MAIR/TCR/SCTLR 配置正确
- ✅ SVC 分发：ESR_EL1.EC=0x15 识别正确
- ✅ EL0 页权限：AP[2:1]=01 (EL0 R/W), UXN/PXN=0
- ✅ MMIO 保护：UART/GIC 使用 AP[2:1]=00 (EL0 不可达)

---

## 统计

| 级别 | 数量 | 需立即修复 |
|---|---|---|
| P0 | 2 | P0-1 (AttrIndx) 最紧迫 |
| P1 | 5 | P1-1/P1-3 优先 |
| P2 | 6 | 可延后 |
