# M3a 修复总结 — ARM64 EL0 入口打通

**日期**：2026-07-17
**目标**：在 ARM64 (QEMU virt) 上实现 EL0 用户态进入 + SVC 系统调用往返

---

## 现象

`enter_el0_test` 输出 `R`（ERET 前的 UART 写入）后无任何后续输出。  
用 QEMU `-d int` 发现 `Exception return to EL0` 成功了，但紧接着触发 `Undefined Instruction`，
且异常进入 `el1h_serror`（`b .`）而非 `el0t_sync`——**两个独立 bug**。

---

## Bug 1：SVC 指令编码错误

### 根因

EL0 测试代码的 `svc #2` 指令编码错误：

```c
// main.c (修复前)
static const uint32_t el0_code[] = { 0xD4000042, 0x14000000 };
```

A64 SVC 编码要求 bits[4:0] = `00001`，但 `0x42` 的 bits[4:0] = `00010`。  
CPU 将此操作码识别为 Undefined Instruction，触发同步异常。

### 修复

```c
// main.c (修复后)
static const uint32_t el0_code[] = { 0xD4000041, 0x14000000 };
```

| 字节 | 修复前 | 修复后 | 说明 |
|---|---|---|---|
| `D4000042` | bits[4:0] = `00010` | — | 非 SVC 操作码 |
| `D4000041` | — | bits[4:0] = `00001` | 标准 SVC 编码 |

---

## Bug 2：异常向量表布局错位

### 根因

`el1h_irq` handler 代码量约 196 字节（SAVE_ALL + IRQ 处理 + RESTORE_ALL），超过 ARM64 向量表单 slot
上限 128 字节。汇编器 `.align 7` 将后续 slot 推至下一个 128 字节边界，导致 `el0t_sync` 实际落
在 offset 0x480 而非标准 0x400。

### 具体偏移对比

| Slot | 预期 offset | 修复前实际 offset | 修复后 offset |
|---|---|---|---|
| `el1h_irq` | 0x280 | 0x280 ✓ | 0x280 ✓ |
| `el1h_fiq` | 0x300 | 0x380 ❌ | 0x300 ✓ |
| `el1h_serror` | 0x380 | 0x400 ❌ | 0x380 ✓ |
| `el0t_sync` | 0x400 | 0x480 ❌ | 0x400 ✓ |
| `el0t_irq` | 0x480 | 0x500 ❌ | 0x480 ✓ |

从 `el1h_irq` 溢出开始，后续 8 个 slot 全部错位。

### 修复

向量表改为 **分支跳板** 模式：

```asm
/* 每个 slot 严格 4 字节：一条 b 指令 + nop 填充至 128B */
VENTRY_BRANCH el1h_irq, el1h_irq_handler
VENTRY_BRANCH el0t_sync, el0t_sync_handler

/* 实际处理函数放在 .text 段，无大小限制 */
el1h_irq_handler:
    SAVE_ALL
    ...
    RESTORE_ALL
    eret
```

`VENTRY_BRANCH` 宏：`.align 7` → `label:` → `b target` → 剩余空间由汇编器 nop 填充。

---

## 辅助修复：中断干扰防护

虽然非根因，但在 `enter_el0_test` 中增加了：

```asm
enter_el0_test:
    msr     daifset, #2      /* mask IRQ */
    ...
    msr     spsr_el1, x0
    msr     elr_el1, x1
    isb
    dsb     sy               /* 确保 MSR 对 ERET 可见 */
    eret
```

防止 timer IRQ 在 `msr spsr_el1` → `eret` 窗口内打断，导致 SPSR_EL1 在 IRQ handler 中被保存/恢复后以错误
的 EL0t 模式做 context switch。

---

## 验证结果

```
R E [SVC] num=2, elr=0x40101104, spsr=0x0
[SVC] SYS_TEST OK
```

- `R` — EL1 中成功打印（ERET 前）
- ERET → EL0 成功
- `svc #2` 正确识别并触发同步异常
- `E` — `el0t_sync` handler 被正确调用（offset 0x400 已修复）
- `[SVC] SYS_TEST OK` — SVC 分发成功
- ERET 返回 EL0 → `b .` 自旋（预期行为，调度器尚未管理 EL0 线程）

---

## 修改文件清单

| 文件 | 改动 |
|---|---|
| `kernel/arch/aarch64/main.c` | L38：SVC 编码 `0xD4000042` → `0xD4000041` |
| `kernel/arch/aarch64/entry.S` | 向量表重构：slot → 4B 分支跳板，handler 体移至 `.text`，新增 `VENTRY_BRANCH` 宏 |
| `kernel/arch/aarch64/el0_test.S` | 添加 `daifset #2` + `dsb sy` 防护 |

---

## 经验教训

1. **ARM64 向量表 slot 严格 128 字节**：不能让任何 handler 内联超过此限制，否则 `.align 7` 静默破坏布局。分支跳板是最安全的模式。
2. **手写指令编码极易出错**：`svc #2` → `0xD4000041` 而非 `0xD4000042`。优先用内联汇编或 `.inst` 宏，或至少交叉验证 ARM ARM。
3. **QEMU `-d int` 是调试 EL 切换的金矿**：直接暴露 ERET 目标 PC、异常类型、目标 handler 地址。
