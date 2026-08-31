# LaOS ARM64 移植文档

> **历史快照**
>
> 本文是按里程碑累积的移植日记，诸如 ahead 数量、剩余 `#ifdef` 和
> “下一阶段”之类的表述只代表对应章节完成时的状态。当前能力边界见
> [current-limitations.md](../current-limitations.md)，当前分支关系见
> [branch-strategy.md](branch-strategy.md)。

> 最后更新：2026-07-25 | 当前阶段：P4 完成。双架构 virtio-blk + LaFS 真设备 CI 验证、TLB 测试统一、共享代码 #ifdef 收敛、x86_64 测试 parity（rollback/negative/sched-stress/multiuser）、ARM64 已 rebase 到 x86_64 trunk、SYS_SLEEP 已确认实现、per-CPU 中断栈基础设施已分配。

---

## 一、动机与设计哲学

### 1.1 为什么移植 ARM64？

| 目标 | 结论 | 理由 |
|------|------|------|
| 在 Mac M 系列运行 | 不需要 — QEMU x86_64 即可 | 已有 ISO 零改动运行 |
| 验证内核跨架构设计 | ✅ 需要 | 暴露 TSO 隐式假设、架构耦合泄漏 |
| 学习 ARM64 系统编程 | ✅ 需要 | GICv3、弱内存模型、MMIO-only、异常级别 |

### 1.2 架构隔离原则

移植的最高原则：**kernel/ 零 `#ifdef`**。

- 所有架构差异收敛到 `arch_dispatch.h` **一个文件**
- 各架构代码独立目录：`arch/x86_64/`、`arch/aarch64/`
- 每个 arch 头文件提供**完全相同的接口签名**
- kernel/ 中的通用代码通过 `#include "arch_dispatch.h"` 间接引用 arch 头文件

### 1.3 x86 → ARM64 概念映射

| x86_64 | ARM64 | 说明 |
|--------|-------|------|
| GS 段 + `swapgs` | TPIDR_EL1 | per-CPU 指针。x86 用 MSR `IA32_GS_BASE`；ARM64 用系统寄存器 |
| IDT (256 条目) | 异常向量表 (16 条目) | ARM64 固定 4 组×4 类型=16 条目，每项 128B |
| LAPIC Timer | ARM Generic Timer | PPI 30，系统寄存器访问 CNTP_*_EL0 |
| LAPIC EOI | GIC ICC_EOIR1_EL1 + ICC_DIR_EL1 | 分离的 priority drop + deactivate |
| IOAPIC | 无 — GIC 直接路由 | PPI 直接走 Redistributor，无需 IOAPIC |
| `iretq` | `eret` | 同样从 SPSR/ELR 恢复 |
| `inb/outb` | MMIO | PL011 UART 通过 MMIO 访问 |
| `cli/sti` | `msr daifset/daifclr, #2` | DAIF[7] = IRQ 掩码位 |
| CR3 (页表) | TTBR0_EL1 | ARM64 有两级 TTBR，EL0 用 TTBR0，EL1 用 TTBR1 |
| PTE bit7=PS (大页) | bit[1]=0 (block desc) | ARM64 用 descriptor type 区分 block/table |
| PTE bit1=R/W | AP[1]=只读标记 | ARM64 权限用 AP[2:1] 两级编码，writable=AP[1]=0 |

---

## 二、里程碑规划

```
M0: 最小串口输出                     ✅ 完成 (2026-07-14)
M1: 中断 + 时钟 (GICv3 + Timer)      ✅ 完成 (2026-07-14)
M2a: 内存基础 (PMM + VMM + MMU)      ✅ 完成 (2026-07-14)
M2b: 上下文切换 (switch.S)           ✅ 完成 (2026-07-15)
M2c: 抢占调度 + heap/vmm_map 修复    ✅ 完成 (2026-07-15)
M3a: EL0 入口 + SVC 系统调用          ✅ 完成 (2026-07-17)
M3b: EL0 中断处理 + 用户态调度        ✅ 完成 (2026-07-17)
M3c: ELF 加载器 + 用户程序             ✅ 完成 (2026-07-17)
```

每个 milestone 都产生**可编译、可链接**的内核，即使功能不全，保证反馈循环紧密。

---

## 三、M0 — 最小串口输出

**目标：** `ARCH=aarch64 make kernel` 编译成功，QEMU `-M virt` 输出 `"LaOS> "`。

### 3.1 前提条件

```bash
# 交叉编译器
apt-get install -y gcc-aarch64-linux-gnu

# 构建
cd /root/src/LaOS
make ARCH=aarch64 kernel
```

### 3.2 构建系统改动

**Makefile：**
```makefile
ifeq ($(ARCH),aarch64)
    $(MAKE) -f kernel.mk ARCH=$(ARCH) TOOLCHAIN=aarch64-linux-gnu
endif
```

**kernel.mk：**
- ARCH 支持：已在 `$(filter ... aarch64 ...)` 中
- CFLAGS：`-mcpu=generic -march=armv8-a+nofp+nosimd -mgeneral-regs-only`
- LDFLAGS：`-m aarch64elf`
- LDFLAGS_POST：`-L /usr/lib/gcc-cross/aarch64-linux-gnu/12 -lgcc`

**链接脚本 (third_party/linker-scripts/aarch64.lds)：**
```
OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)
. = 0x40000000;    # QEMU virt RAM 基址
.text.boot 放最前面
kernel_end = .;     # 供 PMM 避开内核占用区域
```

### 3.3 新增文件（25+ 个文件）

| 文件 | 内容 |
|------|------|
| `serial_arch.h` | PL011 UART MMIO：基址 0x09000000，115200 8N1 |
| `arch_irq.h` | DAIF 操作原语（与 x86 `cli/sti` 接口一致） |
| `arch_barrier.h` | DMB/DSB/ISB 屏障 |
| `arch_cpu.h` | `rdtsc()`→CNTVCT_EL0, `arch_read_cr2()`→FAR_EL1, `arch_cpu_apic_id()`→MPIDR_EL1 |
| `arch_tlb.h` | TLBI VAE1IS/VMALLE1IS + DSB/ISB（M2a 填充） |
| `thread_arch.h` | `arch_get_entry_func()` via x19 走私（M2b 填充） |
| `vmm_arch.h` | PTE 标志位、PTE_IS_LEAF、VMM_TABLE_EXTRA_FLAGS、arch_mmu_init_identity()（M2a 填充） |
| `entry_arch.h` | 空桩（`__builtin_unreachable()`） |
| `cpu.h` | `cpu_context` 结构体 + `cpu_get_ctx()` via TPIDR_EL1 |
| `gdt.h` | 空桩（`USER_CS_SEL=0` 等） |
| `debug.h` | `panic()` + `interrupts_enabled()` |
| `idt.h` | `interrupt_frame` + `EOI()` → GIC EOI（M1 填充） |
| `lapic.h` | `lapic_eoi()` + `timer_init()` + `lapic_init()` → GIC/Timer（M1 填充） |
| `ipi.h` | `IPI_VECTOR_TLB=253` + 空桩 |
| `ist.h` / `acpi.h` / `syscall.h` | 空桩 |
| `arch_aarch64.h` / `arch_x86.h` | `WARN_ON`、`dump_stack` 最小实现 |
| `startup.S` | `_start`：EL3→EL2→EL1 降级链、设 SP、清 BSS、设 TPIDR_EL1 |
| `switch.S` | `switch_to` + `ret_from_fork`（M2b 填充） |
| `entry.S` | 2KB 对齐异常向量表（M1 新增） |
| `main.c` | `kernel_main()`：完整初始化序列 |
| `cpu.c` | `g_cpu_contexts[]` + `g_cpu_count` + `online` 定义 |
| `gic.c` / `gic.h` | GICv3 初始化（M1 新增） |
| `idt.c` | 中断分发 + 抢占调度入口（M1/M2c 填充） |
| `asm_offsets.c` / `ipi.c` | 占位 |

### 3.4 kernel/ 共享代码修改

| 文件 | 修改 |
|------|------|
| `kernel/cpu.h` | 添加 `#elif defined(__aarch64__)` 分发到 `arch/aarch64/cpu.h` |
| `kernel/debug.h` | 添加 aarch64 分发 |
| `kernel/printf.c` | `inb`/`outb` 用 `#ifdef __x86_64__` 守卫；`kprintf_color` 在 TTY 未就绪时不 panic |
| `kernel/arch_dispatch.h` | 解注释 aarch64 `#include`，新增 vmm_arch.h 派发 |
| `kernel/pmm.h` | 新增 `pmm_memmap_entry`/`pmm_memmap` 通用接口 + `pmm_init_from_memmap()` |
| `kernel/pmm.c` | `pmm_init()` 退化为 Limine→通用转换 wrapper |
| `kernel/vmm.h` | PTE 标志位删除，改由 `arch_dispatch.h` 间接引入 |
| `kernel/vmm.c` | 使用 `PTE_IS_LEAF` + `VMM_TABLE_EXTRA_FLAGS` + `VMM_LEAF_EXTRA_FLAGS`（ARM64 heap 修复） |
| `kernel/hhdm.c` | 允许 `offset=0`（ARM64 identity mapping） |
| `kernel/rcu.c` | NULL `current` 守卫（ARM64 早期启动无线程） |
| `kernel/thread.c` | ARM64 栈帧锻造（`#ifdef __aarch64__`） |
| `kernel/heap.c` | `vmm_is_mapped` 预映射跳过 + `hhdm.h` 引入 |

---

## 四、M1 — 中断 + 时钟

**目标：** Timer PPI 以 100Hz 触发，内核每秒在串口打印 uptime。

### 4.1 QEMU 调用

```bash
qemu-system-aarch64 -M virt,gic-version=3 -cpu max -m 512M \
  -kernel bin-aarch64/kernel -nographic
```

必须使用 `-cpu max`（非 `cortex-a57`），Cortex-A57 缺少 GICv3 系统寄存器接口。

### 4.2 GICv3 初始化（关键踩坑）

#### RD 页面地址

```
GICD:  0x08000000  (Distributor)
GICR:  0x080A0000  …这是 SGI 页面！
       +0x10000 = 0x080B0000 …这才是 RD 页面
```

#### GICD_CTLR 使能位

必须设置 bit 1 (`EnableGrp1NS`)，不是 bit 2 (`EnableGrp1S`)：PPI 30 经 `GICR_IGROUPR0` 配置为 Group 1 Non-Secure。

#### 初始化顺序

```
Wake (WAKER.ProcessorSleep=0)
  → wait ChildrenAsleep=0
  → ICPENDR0 = 0xFFFFFFFF  (清除所有 pending PPI)
  → IGROUPR0 |= (1<<30)    (PPI 30 → Group 1 NS)
  → CTLR = 1               (使能 redistributor — QEMU 上不生效)
  → ISENABLER0 |= (1<<30)  (使能 PPI 30)
```

每步后必须 `dsb sy`。

### 4.3 ARM Generic Timer

```
PPI 30 = Non-Secure Physical Timer
CNTFRQ_EL0 = 62.5 MHz (QEMU 默认)
TVAL (100Hz) = 62_500_000 / 100 = 625,000
```

初始化序列：
```c
msr cntp_ctl_el0, xzr          // 禁用
msr cntp_tval_el0, tval         // 写初值
msr cntp_ctl_el0, 1             // 使能
```

**必须在 handler 中重新写 TVAL**（单次触发，不重写会中断风暴）。

### 4.4 EL2 降级

QEMU `-M virt` 默认从 EL2 启动。ICC_SRE_EL1 只能从 EL1 访问：

```asm
el2_entry:
    mov  x1, #(1 << 31)           /* HCR_EL2.RW = AArch64 for EL1 */
    msr  hcr_el2, x1
    adrp x1, el1_entry
    msr  elr_el2, x1
    mov  x1, #0x3c5               /* EL1h + DAIF masked */
    msr  spsr_el2, x1
    eret
```

---

## 五、M2a — 内存基础（PMM + VMM + MMU）

**目标：** PMM bitmap 分配器可工作，identity 映射页表建立、MMU 开启，GIC/timer 继续工作。

### 5.1 前置补全

M2a 之前先补全了 kernel/ 层的跨架构接口：

- **PMM 通用 memmap**：`pmm.h` 新增 `pmm_memmap_entry`/`pmm_memmap` + `PMM_MEMMAP_*` 常量 + `pmm_init_from_memmap()`；`pmm_init()` 退化为 x86_64 Limine→通用转换 wrapper
- **VMM PTE 标志位 arch 化**：创建 `arch/x86_64/vmm_arch.h`、`arch/aarch64/vmm_arch.h`、`arch/riscv64/vmm_arch.h`；`vmm.h` 删除本地 `#define`，改为 `#include "arch_dispatch.h"`

### 5.2 ARM64 mmu_init_identity

实现 `arch/aarch64/vmm_arch.h` 中的 `arch_mmu_init_identity()`：

- 配置 **TCR_EL1**（4KB granule, 48-bit VA, inner shareable, WBWA cacheable）+ HA（硬件 AF 管理）+ HD
- 配置 **MAIR_EL1**（Attr0=device-nGnRnE, Attr1=normal WBWA）
- 分配 4 页表：L0 → L1 → L2_mmio + L2_dram
- L2_mmio[64]：GIC 0x08000000 2MB block device
- L2_mmio[72]：PL011 0x09000000 2MB block device
- L2_dram[0]：DRAM 0x40000000 2MB block normal WBWA
- 设置 TTBR0_EL1 + TLBI + 开 SCTLR_EL1.M/C/I
- 设置 `kernel_pml4 = l0`（供 vmm_map 操作）

### 5.3 kernel/ 兼容性修复

| 文件 | 问题 | 修复 |
|------|------|------|
| `kernel/hhdm.c` | `offset=0` 被 reject | 独立 `s_hhdm_initialized` 标志 |
| `kernel/printf.c` | `kprintf_color` TTY 未就绪 panic | 跳过 fb 输出（对齐 `kprintf`） |
| `kernel/rcu.c` | `current==NULL` 访问崩溃 | NULL 守卫 |
| `aarch64.lds` | PMM bitmap 覆写内核 | 新增 `kernel_end` 符号，memmap 从内核后开始 |

### 5.4 QEMU 启动输出

```
================================
  LaOS — aarch64 (ARM64)  M2
================================
[1] HHDM init (identity mapping)...
[2] Log init...
[3] PMM init (QEMU virt, 512 MB DRAM @ 0x40000000)...
[    pmm] PMM initialized.
[4] VMM: identity mapping + MMU enable...
     MMU enabled (TTBR0_EL1 + SCTLR_EL1.M)
[5] idt_init...
[6] lapic_init...
[7] irq_enable...
LaOS> IRQ enabled, timer running (100 Hz).
```

---

## 六、M2b — 上下文切换（switch.S + thread.c）

**目标：** `switch_to` 在 ARM64 上正确保存/恢复 callee-saved 寄存器，boot 线程 → idle 线程切换成功。

### 6.1 switch.S 寄存器约定

ARM64 callee-saved：x19-x28 + x29(FP) + x30(LR) + SP = 12 寄存器。

栈帧布局必须匹配**恢复顺序**（ldp x29,x30 最先，ldp x19,x20 最后）：
```
[ 0] x29=0 (FP term)   [ 8] x30=ret_from_fork
[16] x27=0              [24] x28=0
[32] x25=0              [40] x26=0
[48] x23=0              [56] x24=0
[64] x21=0              [72] x22=0
[80] x19=entry_func     [88] x20=data
```

**关键坑：恢复顺序与保存顺序相反**。伪造帧时按恢复顺序排，而非保存顺序。

### 6.2 thread.c ARM64 帧锻造

伪代码：
```c
uint64_t *frame = (uint64_t *)(stack_top - (12 * 8));
frame[0]  = 0;                        // x29 (FP termination)
frame[1]  = (uint64_t)ret_from_fork;  // x30 (LR)
frame[2..9]  = 0;                     // x27..x22
frame[10] = (uint64_t)entry_func;     // x19 → thread_entry_point
frame[11] = data;                     // x20 → thread data
```

**线程入口通过 x19 走私**：`thread_arch.h` 中 `arch_get_entry_func()` 读 `t->rsp` 对应偏移的 x19。

### 6.3 关键 bug 修复

| Bug | 根因 | 修复 |
|-----|------|------|
| 栈帧布局错位 | 伪造帧按 x19→x30 排，但 switch.S 先恢复 x29,x30 | 改为 x29→x30→…→x19→x20 匹配恢复顺序 |
| `str sp, [x0]` 非法 | ARM64 不允许 SP 直接做 str 源 | `mov x2, sp; str x2, [x0]` |
| `list_add_tail` NULL deref | BSS 中 runqueue.head.prev/next=0 | `list_init(&ctx->runqueue.head)` |
| `kernel_pml4=NULL` | ARM64 未调 vmm_init | `arch_mmu_init_identity` 设 `kernel_pml4=l0` |
| 表描述符缺 bit[1] | `__vmm_map` 只设 PTE_PRESENT|PTE_WRITABLE | 加 `VMM_TABLE_EXTRA_FLAGS=2` |
| 1GB block 被误当表走查 | `__vmm_map` 不识别 ARM64 block descriptor | 改 L1[1] 为 TABLE descriptor+按需 L3 |

---

## 七、M2c — 抢占调度 + heap 修复

**目标：** Timer ISR 触发 `check_need_schedule()` → `schedule()` 抢占，多线程交替运行，kmalloc/kfree 全链路打通。

### 7.1 抢占调度

`arch/aarch64/idt.c` 的 `irq_handler` 在 `timer_handler` 后加：
```c
if (check_need_schedule()) {
    schedule();
}
```

`timer_handler` 每 100ms 置 `need_resched`，下一个 timer tick 的 IRQ 返回路径走调度。

### 7.2 Heap/VMM 修复（关键架构设计问题）

#### 根因一：`PTE_WRITABLE` 语义错误

| 架构 | PTE_WRITABLE 定义 | 实际含义 |
|------|-------------------|----------|
| x86_64 | bit1 = R/W | R/W 权限位 |
| **ARM64（旧）** | **bit7 = AP[1]** | **只读标记！** (AP[2:1]=10=EL1 R/O) |
| ARM64（修复后） | **0** | writable = AP[1]=0，无需设任何 bit |

ARM64 AP[2:1] 编码：
- `00` → EL1 R/W, EL0 no access
- `01` → EL1 R/W, EL0 R/W（`PTE_USER` 设 bit6=AP[2]）
- `10` → EL1 R/O ← 旧代码实际产生的！
- `11` → EL1 R/O

此外，在 TABLE 描述符上设 bit7 还会设置 **APTable[1]**，使**整个子树只读**。

#### 根因二：`entry & 0x80` 检测大页不兼容

x86_64 用 bit7=PS 检测 huge page leaf；ARM64 用 bit[1]=0 区分 block descriptor。

| 位置 | 旧代码 | 新代码 |
|------|--------|--------|
| `vmm_is_mapped` | `entry & 0x80` | `PTE_IS_LEAF(entry)` |
| `vmm_get_phys` | `entry & 0x80` | `PTE_IS_LEAF(entry)` |
| `__vmm_map` | `entry & PTE_HUGE` | `PTE_IS_LEAF(entry)` |

ARM64 `PTE_IS_LEAF(e)` = `valid && !table`。

#### 根因三：缺 AF (Access Flag)

ARM64 要求所有有效 PTE 设 bit10(AF)，x86 硬件自动管理。引入 `VMM_LEAF_EXTRA_FLAGS=PTE_AF`，在最终 leaf PTE 自动附加。

### 7.3 验证输出

```
[8] kheap_init (KHEAP_VBASE=0x40100000)...
[   heap] Initialized kheap with linked list at 0x0000000040100000.
     heap: base=0x0000000040100000, max=0x0000000040102000
[9] Starting scheduler (heap-alloc idle + test)...
[test] tick
[test] tick
...  (持续运行，无崩溃)
```

kmalloc 分配 thread struct + 16KB stack 成功，test 线程抢占式运行。

---

## 八、经验总结

### 8.1 架构隔离的价值

x86_64 代码 0 改动即完成 M0/M1/M2，验证了 `arch_dispatch.h` 单点架构分发的设计。kernel/ 中的 ARM64 改动仅限 `#ifdef __aarch64__` 栈帧锻造（thread.c）和 `VMM_TABLE_EXTRA_FLAGS`/`VMM_LEAF_EXTRA_FLAGS`/`PTE_IS_LEAF` 等 arch 化宏。

### 8.2 ARM64 PTE 与 x86 的本质差异

| 特性 | x86_64 | ARM64 |
|------|--------|-------|
| 可写位 | bit1=R/W | AP[2:1] 两级编码，writable=AP[1]=0 |
| 大页检测 | bit7=PS | bit[1]=0 (block descriptor) |
| AF (Accessed) | 硬件自动 | 必须显式设 bit10 |
| 表描述符 | 仅需 bit0 | bit0+bit1 都需设 |
| 权限继承 | 最后一级 PT 决定 | APTable 可限制下层（如 bit7 使子树只读） |

### 8.3 中断调试方法论

从外到内，逐层验证：
1. 外设层：poll ISTATUS 确认 Timer 在工作
2. GIC 层：poll IAR 确认中断已到达 CPU interface
3. CPU 层：最小 entry.S 测试确认异常向量表工作
4. C 代码层：逐步添加功能，每次只加一层

### 8.4 调度器策略

- `check_need_schedule()` 在中断返回路径调度（预抢占式）
- `pick_next` 按 round-robin 选 READY 线程，fallback 到 `ctx->idle`
- idle 线程 WFI 空转，timer 唤醒后可能切换到其他线程
- ARM64 定时器是 one-shot，每次 handler 必须重写 CNTP_TVAL

---

## 九、文件清单（当前状态）

### arch/aarch64/ 完整文件（50 个文件）

```
arch/aarch64/
├── arch_aarch64.h      # WARN_ON, dump_stack
├── arch_barrier.h      # DMB/DSB/ISB
├── arch_cpu.h          # rdtsc, arch_read_cr2, arch_cpu_apic_id
├── arch_irq.h          # DAIF 操作
├── arch_tlb.h          # TLBI VAE1IS/VMALLE1IS + DSB/ISB
├── arch_x86.h          # 转发到 arch_aarch64.h
├── asm_offsets.c       # asm offset 生成
├── asm_offsets_gas.h   # GAS 宏输出
├── asm_offsets_nasm.inc# NASM 宏输出（x86_64 交叉）
├── cpu.c               # g_cpu_contexts, 全局变量
├── cpu.h               # cpu_context, cpu_get_ctx()
├── debug.c             # 异常处理器 ★
├── debug.h             # panic, 异常声明 ★
├── el0_test.S          # EL0 测试代码
├── entry.S             # 异常向量表（分支跳板模式）★
├── entry_arch.h        # arch_enter_usermode
├── fdt.c               # FDT 解析（GIC/串口发现）
├── fdt.h               # FDT 声明
├── gdt.h               # USER_CS_SEL / tss_set_rsp0 桩
├── gic.c               # GICv3 初始化 ★
├── gic.h               # GICv3 系统寄存器 ★
├── idt.c               # 中断分发 + 抢占调度入口 ★
├── idt.h               # interrupt_frame, EOI ★
├── ipi.c               # IPI 桩
├── ipi.h               # IPI_VECTOR_TLB
├── lapic.h             # lapic_eoi, timer_init, lapic_init ★
├── limine_entry.S      # Limine 启动入口 ★
├── limine_main.c       # Limine 路径 kernel_main ★
├── main.c              # 直启路径 kernel_main ★
├── serial_arch.h       # PL011 UART
├── serial.c            # 串口 DTB 发现
├── startup.S           # 启动+EL3→EL2→EL1 降级 ★
├── switch.S            # switch_to + ret_from_fork ★
├── syscall.c           # SVC 分发器 ★
├── syscall.h           # SVC 系统调用号
├── thread_arch.h       # arch_get_entry_func via x19 ★
├── initrd_embed.h      # CPIO initrd 动态加载（build 生成）
├── init_common.h       # 双路径共享初始化声明 ★
├── init_common.c       # 双路径共享初始化实现 ★
├── asm_defs.h          # 汇编常量（UART0_BASE 等）★
├── arch_cache.h        # 缓存维护指令（DC CIVAC 等）★
├── virtio_blk.h        # virtio-blk 块设备头 ★
├── virtio_blk.c        # virtio-blk 轮询驱动 ★
├── virtio_mmio.h       # virtio MMIO 传输层 ★
├── g_its.h             # GICv3 ITS 头 ★
├── g_its.c             # GICv3 ITS 驱动 ★
├── module_arch.c       # ARM64 模块分配器 ★
├── user_vmm.c          # 用户地址空间 MMIO 管理 ★
├── page_fault.h        # 页故障恢复声明 ★
├── page_fault.c        # 页故障恢复实现 ★
└── vmm_arch.h          # PTE 标志位 + arch_mmu_init_identity ★
```

### 修改的 kernel/ 文件

```
kernel/
├── arch_dispatch.h     # vmm_arch.h 派发
├── block_device.h/.c   # 块设备抽象（双架构通用）
├── lafs.h/.c           # LaFS 只读文件系统（无架构依赖）
├── cpu.h / debug.h     # aarch64 分支
├── hhdm.c              # offset=0 允许
├── heap.c              # vmm_is_mapped 预映射 + hhdm.h
├── pmm.h / pmm.c       # 通用 memmap 接口
├── printf.c            # kprintf_color 不 panic
├── rcu.c               # NULL current 守卫
├── thread.c            # arch_thread_init_user_frame（共享，零 #ifdef）
├── vmm.h / vmm.c       # PTE_IS_LEAF + arch 化标志位
├── page_fault.c        # 页故障恢复（双架构共享）
├── elf_loader.c        # ELF 加载（仅 ELF 重定位枚举保留 __x86_64__）
```

### QEMU ARM64 运行命令（当前版本）

```bash
qemu-system-aarch64 -M virt,gic-version=3 -cpu max -m 512M \
  -kernel bin-aarch64/kernel -nographic
```

预期输出：PMM init → VMM identity map → MMU enable → GIC/timer → heap → 调度器 + test 线程 [test] tick 持续输出。

---

## 十、M3a — 用户态 EL0 入口（已完成）

**完成日期**：2026-07-17

M3a 打通了从 EL1 到 EL0 再返回 EL1 的最小通路：

### 成果

- `entry.S`：添加 `el0t_sync` (SVC 处理) 和 `el0t_irq` 向量表条目
- `syscall.c`/`syscall.h`：SVC 分发器，`handle_svc()` 按 ESR_EL1.EC=0x15 识别
- `vmm.c`：2MB block 页表拆分（block splitting），支持逐页控制 PTE_USER
- `el0_test.S`：EL0 测试代码（`svc #2; b .`），编译链接到 `.el0_test` 段
- `main.c`：`enter_el0_test()` — 手动创建 EL0 4KB 页表条目，ERET 到 EL0

### 关键修复（Codex 审查 P0）

| 问题 | 描述 | 修复 |
|------|------|------|
| P0-1 AttrIndx 丢失 | block splitting 后 L3 条目丢失 MAIR 索引 → device memory | `l3[i] = ... \| block_attrs \| VMM_LEAF_EXTRA_FLAGS` 保留全部属性位 |
| P0-2 1GB page panic | x86_64 1GB 页拆分改为 panic | 恢复拆分逻辑，`PTE_TABLE_USER_MASK(flags)` 架构无关 |

### 已知限制

- `el0t_irq` 使用 `VENTRY_UNHANDLED` → timer 中断时 hang（M3b 处理）
- EL0 页表创建使用硬编码遍历（P1-1，待改为 vmm_map）
- `arch_enter_usermode` 仍是 UB 桩（P1-3）

### 验证

```bash
qemu-system-aarch64 -M virt,gic-version=3 -cpu max -m 512M \
  -kernel bin-aarch64/kernel -nographic
```

预期输出：MMU enable → kheap init → EL0 4KB page 创建 → SVC 系统调用 OK → [test] tick → 约 100ms 后 EL0 timer hang（已知限制）。


## 十一、M3b — EL0 中断处理 + 用户态调度（已完成）

**完成日期**：2026-07-17

M3b 实现了 EL0 异常返回路径和用户态调度支持：

### 成果

- `arch_enter_usermode()`：从桩实现为 SPSR+ELR+SP_EL0 设置 + `eret`，进入用户态前清零 x0（argc=0）
- EL0 中断处理：`entry.S` 中 `el0t_irq` 保存/恢复 EL0 上下文
- 用户态抢占：timer 中断触发 `check_need_schedule()` → 抢占用户线程
- `vmm_map` 替代硬编码页表遍历

### 关键修复

| 问题 | 描述 | 修复 |
|------|------|------|
| `TCR_EL1.HD=1` 写权限故障 | DBM bit[51] 必须设 1，否则写操作触发 Permission fault | L3 条目统一设 `PTE_DBM` |
| `SCTLR_EL1.UWXN` | 阻止 EL0 写入可执行页 | 启动时清 bit20 |
| AP 编码错误 | AP[2:1]=10 在 ARM64 中为 EL0 禁止访问，非只读 | fix 见 M3c |

---

## 十二、M3c — ELF 加载器 + 用户程序（已完成）

**完成日期**：2026-07-17

M3c 打通了 **ELF 加载 → EL0 入口 → SVC write → 串口输出 → exit** 完整链路。

### 成果

- **用户程序编译**：`user/Makefile` 支持 `ARCH=aarch64`，`user/crt0_arm64.s`（GAS 入口）+ `user/user.lib_arm64.c`（syscall 包装）+ `user/main.c`（验证程序），交叉编译为 AArch64 ELF
- **ELF 加载器**：`kernel/elf.h` 新增 `EM_AARCH64(183)` + ARM64 重定位类型；`kernel/ksym.c` 支持 `R_AARCH64_ABS64`/`R_AARCH64_RELATIVE`
- **嵌入用户程序**：`xxd -i` 转 ELF 为 C 数组，嵌入 `user_elf_embed.h`，`load_user_elf()` 解析并映射 LOAD 段到用户页表
- **SVC 系统调用**：`SYS_WRITE`（PL011 UART 输出）和 `SYS_EXIT`（标记线程 ZOMBIE → 调度）
- **用户程序**：`user/main.c` 最简化——输出两行验证信息后 `exit(0)`

### ELF 加载流程

```
user/main.c (源码)
  → aarch64-linux-gnu-gcc/ld → user/bin-aarch64/user.elf
  → xxd -i → user_elf_embed.h（嵌入内核）
  → load_user_elf() 解析 ELF header
  → 遍历 PT_LOAD 段，为每页 pmm_alloc + 写 L3 PTE + memcpy 段数据 + memset BSS
  → 映射 16 页用户栈 (0x4F0000-0x500000)
  → 返回 ELF entry 地址
  → elf_user_entry() → arch_enter_usermode(frames) → eret
```

### 关键修复（AP 编码 — M3c 核心踩坑）

ARM64 的 AP[2:1] 编码在 vmm_arch.h 注释中写错了 bit 位，导致多个相关问题：

| 问题 | 表现 | 根因 | 修复 |
|------|------|------|------|
| EL0 只读页不可访问 | `FAR=0x401000` DAbort，AT s1e0r 返回 F=1 | `!(flags & PF_W)` 时只设 `PTE_USER`(bit7)，产物 AP[2:1]=10 = EL0 禁止访问 | 只读段设 `PTE_USER \| PTE_AP1` → AP[2:1]=11 = EL0 R/O |
| `argc` 残留 | 遍历 0x400000 个 argv 元素越界 | `arch_enter_usermode` 中 x0 残留内核栈值 | `mov x0, xzr` 清零 argc |
| 栈边界越界 | 16KB 栈不够放 `big_buffer[8192]` + 局部变量 | 16 页 (64KB) 提供足够栈空间 | 栈段扩到 16 页 |

### 正确 AP 编码速查表

```
AP[2:1] | bit7(PTE_USER) | bit6(PTE_AP1) | EL1   | EL0
--------|----------------|----------------|-------|-------
  00    |       0        |       0        | R/W   | 禁止
  01    |       0        |       1        | R/W   | R/W
  10    |       1        |       0        | R/O   | 禁止  ← 旧代码错误产物
  11    |       1        |       1        | R/O   | R/O   ← EL0 只读正确编码
```

### QEMU 验证输出

```
LaOS — aarch64 (ARM64)  M3c
[1] HHDM init...
[2] Log init...
[3] PMM init (512 MB DRAM @ 0x40000000)...
[4] VMM: identity mapping + MMU enable...
[5] idt_init...
[6] lapic_init...
[7] irq_enable...
[8] kheap_init...
[ELF] loading embedded user.elf (70664 bytes)...
[ELF] machine=183 entry=0x400000 phnum=2
[ELF] LOAD va=0x400000 fs=0x104c ms=0x104c fl=5
[ELF] loaded, entry=0x400000
[9] Starting scheduler (idle + test + user)...
[usr] entering ELF @ 0x400000...
LaOS ARM64 ELF: loaded OK
M3c: ELF -> EL0 -> SVC write -> exit, DONE
[exit] user thread exit code=0
```

### 已知限制

- `SYS_EXIT` 后线程清理不完整（FAR=0x8 空指针），M4 修复
- 用户程序嵌入内核镜像（70KB+），后续改为 initrd/CPIO
- `write()` 忽略 fd 参数，仅输出到 PL011 UART
- 无文件系统、无 sbrk/mmap 等 syscall

这些限制中，SYS_EXIT 清理已在后续提交中通过静态线程保护（`is_idle` 标志）兜底。

---

## 十三、M3c 增强 — create_elf_process + debug.c 异常处理器

**完成日期**：2026-07-14 | **提交**：`e3340a6`

M3c 基础版加载用户 ELF 后，缺少正式的 ELF 进程创建管线和异常诊断能力：

### 13.1 ARM64 create_elf_process

**文件**：`kernel/elf_loader.c`

ARM64 实现利用 TTBR0/TTBR1 分离架构：用户页表挂在 TTBR0_EL1，内核通过 HHDM 直接访问用户页表物理地址加载 ELF 段，无需像 x86_64 那样切换 CR3。

```
create_elf_process(elf_raw, elf_size, name, cpus_allowed):
  1. 解析 ELF header，验证 EM_AARCH64(183)
  2. pmm_alloc 16 页用户栈 (0x4F0000-0x500000)
  3. 遍历 PT_LOAD 段，pmm_alloc 逐页 → 写 L3 PTE → memcpy 段数据 → memset BSS
  4. 应用 R_AARCH64_RELATIVE 重定位（如需要）
  5. 分配 TCB + 内核栈，thread_setup_user_frame 架设帧
  6. user_thread_entry_stub → arch_enter_usermode → eret
```

### 13.2 debug.c 异常处理器

**文件**：`kernel/arch/aarch64/debug.c` / `debug.h`

完整的 ARM64 异常诊断基础设施，替代 M3a 阶段 `b .` 静默 hang：

- **ESR_EL1 解码**：EC 异常类别（40+ 种，含 Data Abort、Instruction Abort、SVC、Undefined Instruction 等）
- **FAR_EL1 解码**：故障地址
- **ISS 解码**：DFSC/IFSC 故障状态码 + WnR（读/写）+ S1PTW（页表遍历）
- **全寄存器 dump**：x0-x30 + SP + PC（ELR_EL1）+ SPSR_EL1
- **栈回溯**：FP 链遍历（最大深度 32）+ 符号解析（kallsyms_lookup）
- 将 `idt.c` 的 `exception_handler` 替换为 `debug.c` 版本

### 13.3 PTE 常量化

同时将 `elf_loader.c` 中硬编码的 PTE 值（`0x600 | 0x2 | 0x100` 等）改为架构常量：
- ARM64：`PTE_MEMATTR_NORMAL`（AttrIndx=1, SH=inner shareable）+ `VMM_LEAF_EXTRA_FLAGS`
- x86_64：`VMM_LEAF_EXTRA_FLAGS`（现有定义不变）

---

## 十四、P2 修复 — 大页释放 + 线程帧抽象

**完成日期**：2026-07-14 | **提交**：`23a2e0f`

### 14.1 vmm_destroy_level 大页释放 bug

**文件**：`kernel/vmm.c`

| 问题 | 旧代码 | 新代码 |
|------|--------|--------|
| 叶节点检测 | `entry & 0x80`（x86 PS 位） | `PTE_IS_LEAF(entry)`（ARM64 用 bit[1]） |
| 释放粒度 | `pmm_free(1 page)` | `pmm_free_pages(addr, count)`，count=512(L2) 或 262144(L3) |
| 地址掩码 | 硬编码掩码 | `PTE_ADDR_MASK` |
| present 检查 | 无 | `PTE_PRESENT` 守卫 |

ARM64 大页释放从错误的单页释放改为按块大小释放，与 x86_64 语义一致。

### 14.2 thread_setup_user_frame 抽象

**文件**：`kernel/thread.h` / `thread.c`

`main.c` 和 `limine_main.c` 中重复的 12-slot 栈帧手动填充代码抽象为 `thread_setup_user_frame()`：

```c
void thread_setup_user_frame(struct thread *t, void *entry, uint64_t stack_top);
```

内部按架构填充 callee-saved 寄存器帧（ARM64: x19-x30+SP=12 regs, x86_64: r12-r15+rbp+rbx=6 regs）。避免两处重复代码，同时为后续多架构扩展留接口。

---

## 十五、P3 清理 — ARM64 僵尸文件

**完成日期**：2026-07-14

ARM64 目录中遗留的 x86 名称空桩文件，在 ARM64 构建树中零引用，直接删除：

| 删除文件 | 原因 |
|----------|------|
| `acpi.h` | ARM64 用 FDT，不编译 ACPI |
| `ist.h` | ARM64 用 SP_EL0/EL1 管理栈，无 IST |

保留文件：
- `gdt.h` — 提供 `USER_CS_SEL` / `tss_set_rsp0` 桩（kernel/ 引用）
- `arch_x86.h` — 转发到 `arch_aarch64.h`（`WARN_ON`，被 `mutex.c` 使用）

---

## 十六、Limine UEFI 启动路径

**完成日期**：2026-07-13 ~ 2026-07-14 | **提交**：`9172bc5` → `c8e41c9`

ARM64 Limine 协议启动，使内核能在 UEFI 固件下以 EL1 启动（而非 QEMU 直启的裸机入口）。复用直启路径初始化函数，差异仅在内存映射和早期启动环境。

### 16.1 与直启路径的差异

| 方面 | 直启（`-kernel`） | Limine（ISO UEFI） |
|------|-------------------|---------------------|
| 入口异常级别 | 自定义 EL3→EL2→EL1 降级链 | 固件已设 EL1（或 EL2，限 Limine 8.x） |
| SP_ELx 来源 | 汇编直接 `mov sp, x0` | **SP_ELx 由固件设定**，只能用 `mov sp` + SPSel 切换 |
| 内存映射 | 硬编码 DRAM 0x40000000 起 | Limine memmap 提供，可能包含 UEFI runtime 保留区 |
| 页表大小 | identity 映射 4 页表（L0→L3） | minimal 页表仅 L0+L1+L2（无 L3），节省启动内存 |
| 设备树 | 无 | Limine 提供 DTB 指针 |
| 用户栈顶 | `0x500000` | lowmem 限制需更低地址 |

### 16.2 关键踩坑

#### EL1t 入口

Limine 以 **EL1t**（SPSel=0，共享 SP_EL0）进入内核。异常向量必须处理 EL1t 同步/IRQ 入口——复用 EL1h handler，但需在 handler 内部判断 SPSel 并做栈切换。

#### SP_ELx 写 trap

`MRS/MSR SP_ELx` 在 UEFI 环境下 trap EC=0x00（未定义）。必须用 `mov sp, ...` 操作栈指针。`startup.S` 改为：`SPSel #1` 切换到 SP_EL1 + `mov sp, x0` 设栈，而非 `msr sp_el1, x0`。

#### 最小页表 + PTE_AF

Limine 入口只建立 3 级页表（L0+L1+L2），不分配 L3。必须在 L2 block descriptor 上设 `PTE_AF`（bit10），否则 ARM64 硬件触发 Access Flag fault。直启路径因有 L3 拆分，AF 通过 `VMM_LEAF_EXTRA_FLAGS` 自动附加。

#### Makefile 模块 ARCH 传递

模块编译时 Makefile 未传递 ARCH 给 `kernel.mk`，导致模块用 x86_64 工具链编译 ARM64 内核。修复：顶层 `Makefile` 中 `$(MAKE) -f kernel.mk ARCH=$(ARCH)`。

### 16.3 构建 + 运行

```bash
# 构建 Limine ISO
make ARCH=aarch64

# QEMU UEFI 启动
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a57 -m 512M \
  -drive if=pflash,format=raw,file=third_party/limine/BOOTAA64.EFI,readonly=on \
  -cdrom build/LaOS-arm64-limine.iso -nographic
```

### 16.4 验证结果

```
LaOS — aarch64 (ARM64)  M3c (Limine)
[1] HHDM init (Limine offset=0xFFFF800000000000)...
[2] Log init...
[3] PMM init (from Limine memmap)...
[4] VMM: identity mapping + MMU enable...
[5] idt_init...
[6] lapic_init...
[7] irq_enable...
[8] kheap_init...
[ELF] loading embedded user.elf...
[ELF] loaded, entry=0x400000
[9] Starting scheduler...
[usr] entering ELF @ 0x400000...
LaOS ARM64 ELF: loaded OK
M3c: ELF -> EL0 -> SVC write -> exit, DONE
[exit] user thread exit code=0
```

该阶段曾验证双路径均到达 M3c DONE。M3d 改为公共 `task.conf` 管线后，验证状态见下一节。

---

## 十七、M3d — Limine task.conf + ARM64 动态模块

**完成日期**：2026-07-18

Limine ISO 现在显式传入三个 boot module：`/conf/task.conf`、`/task/user.elf`
和 `/task/module_foo.mo`。ARM64 不再依赖 `limine_main.c` 中的嵌入式 ELF
旁路，而是执行公共的 `module_alloc_init()` → `task_init()` → `task_run()` 管线。

`task.conf` 已被规范为 LaOS 的 boot-time orchestration DSL。当前 v1 语法
保持 `CPU MODULE[:ENTRY] TYPE MAGIC [ARGS...]` 向后兼容，后续 `@test`
directive 和测试 `.mo` 模块化设计见 [`task-conf-dsl.md`](../task-conf-dsl.md)。

### 17.1 模块地址与编译模型

- x86_64 继续使用固定的 `0xffffffffc0000000` 模块区域。
- ARM64 根据当前内核地址选择 2MB 对齐的近内核窗口，大小 64MB、起点距内核约
  32MB，使 `R_AARCH64_CALL26` 保持在 ±128MB 范围内。
- 直启路径为 TCR 配置 T1 参数，并让 TTBR0/TTBR1 共享页表根；Limine 路径复用
  已接管的高半区页表根。
- 模块使用 `-fno-PIC -mgeneral-regs-only`，加载器支持 `CALL26/JUMP26`、
  `ADRP+LO12`、ABS/PREL 等实际生成的 AArch64 重定位，并在修改代码后同步
  D-cache/I-cache。

### 17.2 验证

```bash
./run.sh auto
# 或：make test-arm64-limine
```

关键输出：

```text
[task] queued foo from module_foo.mo (type=1)
[task] queued user0 from user.elf (type=3)
[module-foo] started: count=2 tick=10
PASS: aarch64 Limine task.conf + dynamic module chain
```

这证明 Limine 模块传递、配置解析、模块 VA 映射、符号解析、重定位、模块入口和
`MODULE_PARAM` 参数写回均已生效。

### 17.3 M3d+ 连续调度修复

强验收最初暴露的停滞并非 `switch_to` 选择错误，而是切换到私有 TTBR0 后，EL1
仍通过低地址 `0x09000000` 访问 PL011，导致 translation fault；异常诊断又被
`kprintf` 的全局 `print_lock` 遮蔽。修复包括：

- 每个 ARM64 用户页表映射 supervisor-only、device 属性的 PL011 页；EL0 无权直接访问。该映射通过 `arch_user_vmm_init/destroy` 平台接口管理，公共 ELF/线程代码不再硬编码 PL011 地址。
- Limine 正式 `kernel_pml4` 继承 early low-half MMIO 表，用户退出切回内核根后 UART 仍可访问。
- 用户地址空间销毁前先解除非 PMM 所有的 UART 映射，并用 `PTE_ADDR_MASK` 提取叶物理地址。
- 模块验收标志与 `SYS_EXIT` 提示走无锁串口，异常/退出路径不再依赖 framebuffer/TTY 锁。

最终强验收要求并已观察到：

```text
[module-foo] started: count=2 tick=10
[module-abi] relocation + data + bss, PASS
M3c: ELF -> EL0 -> SVC write -> exit, DONE
[exit] user thread exited
[module-foo] resumed after timeout
PASS: aarch64 Limine module + EL0 task chain
```

`module_abi.mo` 是专用的加载器回归固件。构建阶段用 `readelf` 确认其实际
包含 `ABS64`、`CALL26/JUMP26`、`PREL32`、`ADRP+LO12`、
`LDST8/32/64_LO12` 和 `NOBITS .bss`；
运行阶段再校验初始化数据、零填充 BSS、本地函数调用、内核导出符号和
绝对函数/数据指针。

负向回归 `make test-arm64-limine-negative` 会使用独立 task.conf 加载
引用不存在内核符号的 `module_bad.mo`。验收要求出现明确的模块加载
失败标志，之后 ABI 模块、`module_foo` 和 EL0 任务仍全部运行。
加载器在调用 bump 式 `module_alloc()` 前预检运行段的重定位类型和所有
未定义符号，因此这类可确定的加载失败不会消耗模块 VA 区域。

直启回归 `make test-arm64` 会轮询 EL0 exit 与网卡探测标志，成功后主动
结束 QEMU；内核后续持续输出 tick 不会再让测试固定等待 120 秒。
Limine 回归也会在模块启动、EL0 exit 和模块恢复三个标志齐备后立即回收
QEMU。ISO 使用 Limine 模板的 EFI boot-partition 布局，已无需键盘注入即可自动启动。

---

## 十八、M4 初期 — ARM64 Limine SMP bring-up

**当前状态**：2026-07-18

本阶段目标是把 x86_64 的 Limine SMP 启动模型迁移到 ARM64：

```text
AP handoff -> per-CPU context -> GIC redistributor -> local timer -> online -> scheduler
```

已完成并验证的内容：

- Limine MP handoff 可启动所有 AP，并把稀疏 MPIDR 映射为连续逻辑 CPU id。
- AP 在 `limine_ap_park()` 中建立独立 `cpu_context`、静态 idle TCB、AP kernel stack、
  runqueue/zombiequeue，并发布到 `g_cpu_contexts[id]`。
- GIC redistributor 按 `MPIDR_EL1` affinity 匹配，不再依赖固定 stride 顺序。
- AP 可在 BSP 控制下完成 per-CPU GIC init，然后等待 release mask。
- BSP 建好 CPU0 scheduler/task 队列后释放所有 AP，避免早期 loader 与 AP 并发。
- AP release 后初始化本地 ARM Generic Timer，进入 idle scheduler。
- `g_cpu_count` 和 `online` 在 4 vCPU 验证中可达到 `4/4`。
- AP 可按 `task.conf` 的 CPU 绑定启动 `module_smp_probe.mo`，2 CPU 和
  4 CPU 验证均覆盖 `[smp-probe] cpu=N`。
- x86_64 已先修正 LAPIC ID 与逻辑 CPU id 混用问题，ARM64 复用同样的“硬件 id
  与 per-CPU 数组下标分离”原则。

自动验证命令：

```bash
bash run.sh smp
SMP_CPUS=4 bash run.sh smp
```

关键验收标志：

```text
[smp] APs parked: 3/3
[smp] AP GIC ready: 3/3
[smp] AP online: 3/3 (global=4/4)
[smp-probe] cpu=1
[smp-probe] cpu=2
[smp-probe] cpu=3
[smp] AP online: 3/3 (global=4/4)
[module-foo] started: count=2 tick=10
M3c: ELF -> EL0 -> SVC write -> exit, DONE
[module-foo] resumed after timeout
PASS: aarch64 Limine module + EL0 task chain
```

### 18.1 与 x86_64 的对齐程度

已经对齐：

- AP 有独立 per-CPU context，而不是共享 BSP context。
- AP 有自己的 idle 线程和 runqueue。
- AP 有自己的中断向量、GIC CPU interface/redistributor 和 local timer。
- AP 可以进入 `schedule()`，CPU0 任务链在 2 核和 4 核下继续通过。
- AP 可以按 `task.conf` 启动绑定到指定 CPU 的模块任务；`bash run.sh smp`
  覆盖 2 CPU，`SMP_CPUS=4 bash run.sh smp` 覆盖 CPU1/2/3 的
  `module_smp_probe.mo`。
- BSP 可以向 AP 广播 `IPI_VECTOR_TLB`，AP 侧 GIC SGI handler 会执行
  `arch_tlb_flush_all()` 并 ack；`bash run.sh smp-tlb` 覆盖 2 CPU，
  `SMP_CPUS=4 bash run.sh smp-tlb` 覆盖 4 CPU。
- 跨 CPU TLB shootdown 已从 ack smoke 推进到 DSL 驱动的 remap 可见性验证：
  `@test smp_tlb_remap rounds=N` 让 BSP 把同一测试 VA 在 page A/page B 间
  循环 remap，AP 在 IPI handler flush 后读取该 VA，必须看到每轮新值。
- 模块加载失败路径已有 `module_alloc` checkpoint/rollback；`bash run.sh rollback`
  通过 `module_no_entry.mo` 验证失败事务回收后，后续正常模块和 EL0 任务链仍能继续运行。

尚未完全对齐：

- ARM64 AP 当前只稳定验证到 IRQ-off smoke module；未把通用 mutex/preempt
  压力线程打开到 AP。
- AP task module 先原子标记、再由 BSP timer 串行输出 `[smp-probe] cpu=N`。
  这避免 AP 在释放阶段直接使用 console/framebuffer 路径。
- 跨 CPU TLB 已验证多轮 remap 可见性；还没有做 unmap 压力，也没有让 AP
  普通线程与 BSP 同时制造页表更新竞争。
- 模块 loader / `module_alloc` 已能做串行加载事务回滚；真正的模块卸载、
  脱离 `task_lock` 的任意并发分配，以及更高压力的并发验证仍未完成。

### 18.2 已解决实验记录

`module_smp_probe.mo` 已加入 `conf/task-arm64.conf`：

```text
1    module_smp_probe.mo:smp1    1    0xd
2    module_smp_probe.mo:smp2    1    0xe
3    module_smp_probe.mo:smp3    1    0xf
```

预期输出：

```text
[smp-probe] cpu=1
[smp-probe] cpu=2
[smp-probe] cpu=3
```

已通过：

- `bash run.sh smp`
- `SMP_CPUS=4 bash run.sh smp`
- `bash run.sh smp-tlb`
- `SMP_CPUS=4 bash run.sh smp-tlb`
- `bash run.sh rollback`

本轮修复点：

- ARM64 `ret_from_fork` 不再依赖 C 代码从 x19 读入口函数，而是在汇编里把
  x19 搬到 x1，作为 `thread_entry_point(data, entry_func)` 的显式参数。
- ARM64 `ret_from_fork` 会在进入内核线程入口前切到 SP_EL1，避免 AP idle
  开 IRQ 后异常入口使用过期 SP_EL0。
- task.conf 模块入口不再用 `entry_argc > 0` 判断；无参数模块也必须走
  `main(argc, argv)` 路径。用户线程通过 `is_user` 排除，继续走 EL0 trampoline。
- 新增 `smp-tlb` 验证入口，使用 CPU0-only `task.conf` 保持 AP idle/IRQ-on，
  并通过 `@test smp_tlb_remap rounds=4` 显式启用 TLB remap stress。
  通过 `[smp] SGI TLB ack: N/N` 检查跨 CPU TLB IPI 链路，并通过
  `[smp] TLB remap visible: N/N rounds=4` 检查 remap 后 AP 可见新物理页内容。
- 新增模块加载事务 checkpoint/rollback，失败路径回收本次 ET_REL 加载产生的
  模块 VA、物理页和映射；`module_no_entry.mo` 用于固定触发回滚验证。

### 18.3 后续推进顺序

1. 把跨 CPU TLB 从多轮 remap 可见性推进到 remap/unmap 混合压力测试。
2. 继续确认模块加载器、kheap 和 task list 遍历在更高并发下的 SMP 安全性。
3. 打开 AP 上的 `mutex_test_start_thread()` / preempt 测试，验证跨 CPU 调度压力。
4. 最后补齐设备中断和 RCU/monitor 在多 CPU 下的 ARM64 回归。

---

## 十九、P4 — 双架构设备抽象 + CI 完善（已完成）

**完成日期**：2026-07-23

本阶段将 ARM64 专有的存储、测试能力抽象为双架构共享代码，补齐 x86_64 的块设备与文件系统支持。

### 19.1 成果

- **x86_64 virtio-pci 传输层**：`kernel/arch/x86_64/virtio_pci.c/h` — PCI 能力链扫描、legacy virtio transport、描述符链管理、notify 机制。
- **block_device 通用抽象**：`kernel/block_device.h/c` — 统一块设备注册表，支持 stub 设备隔离单元测试。
- **LaFS 通用化**：`kernel/lafs.c/h` — 零架构依赖的只读文件系统，两架构共享。
- **CI 真设备验证**：`test-x86_64-lafs` 和 `test-arm64-lafs` 已纳入 `test-all`，`block_device_reset()` 保证测试隔离，验证真实 virtio 设备挂载 + `/etc/motd` 读取。
- **x86_64 TLB shootdown 压测**：`test_tlb.c` 双架构统一，`test-x86_64-smp-tlb` 接入 CI，覆盖 4 CPU remap 可见性。
- **共享代码 `#ifdef` 收敛**：`__aarch64__` guard 清零，`__x86_64__` 仅剩 1 处（ELF 重定位枚举 `R_386_*`）。block_device/LaFS/elf_loader 等核心模块均无架构 guard。
- **分支同步**：`arm64` 已 rebase 到 `x86_64` trunk，0 ahead commits，共享代码完全同步。

### 19.2 P0 硬 Bug 修复

| Bug | 根因 | 修复 |
|-----|------|------|
| desc_head 链错乱 | 每次 `submit_virtqueue` 重新扫描空闲描述符 | 固定 `desc_head=0`，保证描述符链一致 |
| notify multiplier 错误 | struct cast 截断 PCI config space 多字节值 | 从 PCI config ptr+16 直接读取 4 字节 |
| test-x86_64-lafs 假阳性 | stub 设备覆盖真实设备 | 单元测试提前 + `block_device_reset()` + 真实 virtio 挂载验证 |
| DMA 地址错误 | `virt_to_phys()` 假设 HHDM 映射，kmalloc 不保证物理连续 | `pmm_alloc` + `phys_to_virt`，单页容纳全部 virtqueue 结构 |
| 特性协商缺失 | 未写 VIRTIO_F_VERSION_1，未验证 FEATURES_OK | 协商 VERSION_1，读回确认设备接受 |
| 轮询超时静默 | 提交前未设 sentinel，超时不返回失败 | status byte 预设 `0xFF`，超时返回 -1 |

### 19.3 x86_64 测试 parity

- `test_sched_stress.mo` 提升为双架构模块（原 ARM64 专有）
- 新增 4 个 x86_64 test target：`rollback`、`negative`、`sched-stress`、`multiuser`
- `@module_missing` 已纳入 `check_task_conf_v1.sh` 校验白名单
- `make test-all` 全链路通过（不含 riscv64 编译）

### 19.4 x86_64 中断栈基础设施 + SYS_SLEEP 确认

- **SYS_SLEEP (syscall 35)**：经审计确认 x86_64 早已完整实现——`sys_msleep()` → `schedule_timeout()`、`user/lib.c` 封装 `msleep()`、`user/main.c` 实际使用 `msleep(5000)`。此前功能对比误判为缺失。
- **per-CPU 中断栈基础设施**：`cpu_context` 新增 `int_stack` / `int_stack_base` / `saved_rsp` 字段，`INT_STACK_SIZE` (8KB) 定义于 `thread.h`（双架构共享），`per_cpu_init()` 为所有 CPU 分配中断栈（BSP 另有静态 BSS fallback），`asm_offsets` 自动生成 `CTX_INT_STACK` 等汇编常量。
- **实际切换延后**：`idt_stubs.S` 的栈切换逻辑因与 `switch_to` 交互复杂暂未启用——调度后新线程恢复时 `RSP` 回自己的内核栈，`POP_ALL` + `iretq` 假设中断帧仍在当前栈顶。ARM64 的 `SAVE_ALL`/`RESTORE_ALL` 封装了整个中断帧使其自然正确；x86_64 需要 per-thread `saved_rsp` 跟踪才能正确处理线程切换场景。

## 二十、待完成工作

- **TODO：模块卸载 / 任意并发分配**：`module_alloc` 已支持加载事务
  checkpoint/rollback，但 `module_free()` 仍为 no-op；真正 unload、引用计数、
  以及不依赖 `task_lock` 的任意并发模块分配仍未实现。
- **M4**：继续验证 ARM64 AP 设备中断、跨 CPU TLB 循环压力和压力调度
- **P1**：继续验证已合入的 shell、TTY、preemption 和 RCU 路径在 ARM64 上的行为
- 其他：共享代码中仅 1 处 `#ifdef __x86_64__`（`elf_loader.c` 的 `R_386_*` 重定位枚举），`__aarch64__` 为 0。
