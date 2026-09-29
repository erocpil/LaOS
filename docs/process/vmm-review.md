# vmm.c 体检报告

文件:`kernel/vmm.c` 922 行 + `kernel/vmm.h` 64 行。

**用途**:vmm 子系统的代码审查与修复施工记录。
**HEAD**:扫描 `d4e8a8b add docs`，V-2 修复落在 `2ca0651 fix vmm`.
**风格说明**:本报告比 pmm-review.md 简洁.pmm 是分阶段大改(3 个 phase / 17 项)，vmm 风险点更集中，不需要等同细致度。

---

## 一，TL；DR

vmm.c 表面接口完备，深读发现：

- **2 处缺陷**(其中 S1 silent corruption，S2 SMP 一致性故障)
- **3 处接口契约不一致**
- **死代码 ~179 行(19%)**，分布于 `#if 0` 与 `#if 1 / #else` 留底块

整体判断：活逻辑约 750 行，规模与 pmm 改造前相当。但缺陷性质比 pmm 危险:`vmm_map_region` 对齐计算错位是 silent corruption，不会立即触发 panic 暴露。

---

## 二，缺陷

### S1 `vmm_map_region` 对齐掩码错位(**P0 silent corruption**)

vmm.c:597-598 原代码：

```c
uint64_t aligned_size = ((end_vaddr + (PAGE_SIZE - 1)) &
        ~((uint64_t)(PAGE_SIZE))) - v_start;
```

`~PAGE_SIZE = 0xFFFFFFFFFFFFEFFF`:bit 12 被清除，掩码语义变为"8KB 对齐再减 4KB"，而非 page-aligned.**正确写法是 `~(PAGE_SIZE - 1)`**.

**实测推算**(PAGE_SIZE=4096):

| size | end_vaddr+4095 | & ~PAGE_SIZE | 期望循环次数 | 实际 |
|---|---|---|---|---|
| 4096 | 8191 | 4095 | 1 | 1 [OK] |
| 4097 | 8192 | 8192 | 2 | 2 [OK] |
| 8192 | 12287 | 12287 | 2 | **3** [X] 多映 1 页 |
| 8193 | 12288 | 12288 | 3 | 3 [OK] |
| 12288 | 16383 | 16383 | 3 | **4** [X] |

规律：当 `end_vaddr` 落在 `[2N.4096, (2N+1).4096)` 区间时多映一页。

**严重性放大器**:vmm.c:446-450 `__vmm_map` 在 `PTE_PRESENT` 已置时调用 `panic("VMM: Overwriting mapping at %p")`.两次相邻 `vmm_map_region` 大映射，若第二次起点落在第一次"多映了 1 页"的位置，会触发 panic.boot 阶段未暴露，是因为 `pci.c` / `lapic.c` 的实际 size 都是 small power-of-2，恰好落在不多映的区间。

修复:`~((uint64_t)(PAGE_SIZE - 1))`.**已落 commit `2ca0651`**.

### S2 `vmm_map_global` 缺 IPI 击落(**P0 / SMP 一致性**)

vmm.c:560-577 原代码：注释写"3. 通知其他核心刷新(IPI 击落)"，实际 line 571 仅调用 `L()` 打印日志，**未发送 IPI**.同文件 `vmm_remap` line 542 已正确使用 `ipi_broadcast(IPI_VECTOR_TLB)`，仅 `vmm_map_global` 遗漏。

本核 TLB 由 `__vmm_map` line 457 的 `invlpg` 处理，line 568 注释化的 invlpg 是冗余写法，不计入缺陷.**问题在其他核**:BSP 调用 `vmm_map_global` 后，AP 上的 TLB 仍持有旧条目。当前 boot log 未暴露，是因为 `vmm_map_global` 现有调用点(IDT / LAPIC 早期映射)均处于 BSP 单核阶段，AP 尚未上线。

修复:line 571 替换为 `ipi_broadcast(IPI_VECTOR_TLB)`.**已落 commit `2ca0651`**.

---

## 三，接口契约不一致

### S3 `vmm_remap` 是 EXPORT_SYMBOL 但 vmm.h 漏声明(**P1**)

vmm.c:533 是公共函数，vmm.h 未声明。调用方依赖 implicit declaration(C99 freestanding 编译器接受)，返回类型默认 int 恰好与定义一致才未触发链接异常。

修复:vmm.h 增加 `int vmm_remap(...)` 声明.**待 V-3 落地**.

### S4 `get_next_level` 非 static 但未在 vmm.h 暴露(**P1**)

vmm.c:358 既无 `static`，亦不在 vmm.h，污染全局符号表。

判定方式:grep 所有调用点：

- 调用点全部位于 vmm.c 内部 -> 加 `static`
- 存在跨文件调用 -> 加入 vmm.h

**待 V-3 grep 后落地**.

### S5 `__vmm_map` 不强制 `PTE_PRESENT`(**P1**)

vmm.c:454:

```c
pt[pt_i] = paddr | flags;
```

`flags` 由调用方传入。如果调用方未包含 `PTE_PRESENT`，整页映射静默失效(访问触发 #PF).处理方式有两种：

- 强制 OR `PTE_PRESENT`:调用方遗漏时兜底，但隐藏潜在缺陷不利于暴露
- 不修改代码，在 vmm.h doc block 中明示契约

倾向:**doc block 明示契约**.理由：契约显式化优于隐式兜底，符合教学语义。

---

## 四，工程性

### S6 死代码 ~179 行(19%)

| 块 | 行号 | 行数 | 内容 |
|---|---|---|---|
| `#if 0` | 89-108 | 20 | `get_next_level` V1 |
| `#if 0` | 110-166 | 57 | `vmm_map` V1 |
| `#if 0` | 327-353 | 27 | `vmm_test_secret_base` V1 |
| `#if 1 / #else` | 686-781 | 96 | `vmm_map` V2 + V3，两侧均为 `//` 注释(无效代码块) |

处理方式：全部删除。

### S7 `vmm_create_user_pml4` 注释化石

vmm.c:632-651 含大量"曾经这样做 / 现在改成这样"的过程性注释，**删除**.

### S8 `vmm_map_region` line 609-610 调试 L() 留底

```c
if (!(offset & ((1 << 20) - 1)))
    L("offset %lx %lx n %d", offset / PAGE_SIZE, offset, n);
```

每 1MB 输出一行，进入生产路径会污染日志。按 pmm.h 风格直接删除。

---

## 五，不在本 phase 范围

- **`vmm_lock` 单全局锁**:所有 map/unmap/remap 串行。等 SMP 工作负载暴露问题再做拆分，当前**不动**.
- **`vmm_test_secret_base` 测试地址 `0x10000000000`**:vaddr 落在用户态范围(PML4 idx 32)，但 `vmm_preallocate_kernel_range` 仅预分配 256-511.能运行，但语义错位。留作 V-3 测试补全时一起处理。
- **`vmm_init_bsp` 拷贝 256-511 + `vmm_preallocate_kernel_range` 再次遍历 256-511**:两个循环职责为"克隆"与"预分配"，可合并；但解耦各自语义清晰(一次性 vs 多核协议)，不强行合并。

---

## 六，原方案中已撤销的条目

### `vmm_init(int flag)` 用 0/非 0 区分 BSP/AP

初版方案曾将其列为 P2 修复项("`flag == 0 巧合是 BSP`，耦合了 cpu_id 编号策略与初始化语义")，主张改为 enum 或拆分为 `vmm_init_bsp() / vmm_init_ap()`.

**结论**:撤销.`flag == 0 <-> BSP` 不是巧合，而是 x86 体系结构 + Limine SMP 协议保证的恒等式：
- 系统上电时只有 BSP 运行(lapic_id=0)
- 被 BSP 通过 INIT-SIPI-SIPI 唤醒的核按定义为 AP
- main.c:148 `info->lapic_id` 由 Limine SMP trampoline 传入，对 AP 永远非 0

原签名 `void vmm_init(int flag)` 利用了该不变量，写法正确。仅 `flag` 变量名容易让读者误读为 mode bit，可在后续重命名为 `cpu_id`，但不属于 V-2 修复范围。

---

## 七，Phase 拆分

**V-1:清死代码 + 头文件契约**(~1h，零风险)
- 删除 S6 全部 179 行
- 删除 S7 / S8 注释与调试 L
- S3 `vmm_remap` 加入 vmm.h
- S4 `get_next_level` 视调用点选择 `static` 或 vmm.h
- 编译通过 + boot log 形态一致即可

**V-2:缺陷修复**(已完成)
- S1 `~PAGE_SIZE` -> `~(PAGE_SIZE - 1)` [PASS] commit `2ca0651`
- S2 `vmm_map_global` 增加 `ipi_broadcast(IPI_VECTOR_TLB)` [PASS] commit `2ca0651`
- S5 doc block 明示 `PTE_PRESENT` 契约(不改代码)-> 移至 V-3
- 增加 `vmm_test_map_region`:跨页边界 + 不同 size -> 移至 V-3

**V-3:文档定稿 + 测试补全**(~30min)
- S3 / S4 / S5 接口契约调整
- pmm.h 风格 doc block 写入 vmm.h
- 增加 `vmm_test_map_region` 边界测试
- 本文档增加"修复实测"小节(含 boot log 校验记录)

---

## 八，修复实测(V-2)

**boot 验证**:`/tmp/vmm-v2.log`(3659 行，QEMU q35 + 4 核 + 2GB).

| 验证点 | 预期 | 实际 |
|---|---|---|
| 不出现 `Overwriting mapping` panic | 字符串不出现 | [PASS] 全日志未命中 |
| 不出现 #PF / #GP / Triple Fault | 无异常 | [PASS] 无任何 fault / panic / assert |
| BSP `vmm_init` 完成 | `VMM Initialized for CPU 0!` | [PASS] L240 |
| 三 AP 全部完成 `vmm_init` | 三行 `VMM Initialized for CPU N!` | [PASS] L808 / L832 / L864(CPU 3 / 2 / 1)|
| `vmm_test_secret_base` 通过 | secret + alias 均读到 `0xCAFEBABE` | [PASS] L233 / L234 |
| `kernel_pml4` 全核一致 | 四核均指向 `0xffff800000110000` | [PASS] |

**AP 完成顺序观察**:CPU 3 -> CPU 2 -> CPU 1，非 1->2->3.原因：三个 AP 通过 Limine MP request 一次性触发 trampoline，并发执行 `secondary_cpu_init` -> `vmm_init`，完成顺序受 Limine 内部启动协议与 QEMU TSC 抖动影响，无序但合法.`vmm_lock` 全局锁将三 AP 的 `vmm_init` 路径串行化，未观察到 deadlock 或 panic.

---

## 九，和其他文档的关系

- 上游:mm-cleanup-plan.md Phase B
- 同期:pmm-review.md 已定稿(commit 397353b)
- 下游:(未来 heap-review.md / vmm 多核拆锁)

---

## 十，周边发现(不在 vmm 修复范围)

讨论 vmm V-2 时附带审查 SMP 启动同步路径，发现以下与 vmm 无关，但值得记录的项：

### S10.1 `wait_online` 计数器非原子(已修)

`kernel/cpu.c:21` 原 `online++` 在多核并发下不是原子(编译为 `add [mem]，1` 无 lock 前缀，或 `mov/inc/mov` 三条指令).四核 boot 窗口窄未触发死锁，但核数增加或 vmm 路径变快即可暴露：任意一次丢更新均导致 `online` 终值 `< g_cpu_count`，所有核永远自旋。

修复：改为 `__atomic_fetch_add(&online, 1, __ATOMIC_SEQ_CST)`;`while` 条件读改为 `__atomic_load_n(&online, __ATOMIC_SEQ_CST)`，显式表达内存序意图，不依赖 x86 TSO happen-to-work.

**实测**(vmm 分支 boot serial.log):

| 验证点 | 结果 |
|---|---|
| 4 行 `Online and standing by`(含 `[CPU 0]`) | [PASS] 齐全 |
| 4 行 `VMM Initialized for CPU N` | [PASS] 齐全 |
| `panic` / `fault` / `overwriting` | [PASS] 0 命中 |

退出 `while` 的顺序:CPU 1 -> CPU 0 -> CPU 3 -> CPU 2，无序但合法(屏障语义只保证全部到达后退出，不保证退出顺序).

### S10.2 `wait_online` 循环内 `L()` 是 print_lock 热点(待办)

`kernel/cpu.c:23` 在自旋循环中每轮调用 `L()`:

```c
while (... != g_cpu_count) {
    L("CPU #%d waiting for cpu online", id);
    __asm__ volatile("pause");
}
```

`L()` -> `kprintf` -> `arch_spin_lock_irqsave(&print_lock)` -> fb_print + serial_puts(serial 走 outb 字节级 I/O，单行约数十微秒).当前 boot log 中 ~2800 行 `CPU #N waiting` 的真实成本不在 `pause`，而在 `print_lock` 抢锁与 serial I/O 队列.`pause` 本身已是 SMT 友好的低功耗 spin(等同 Linux `cpu_relax()`)，不需替换为 `mwait` / `hlt`.

优化方向：循环内 `L()` 节流：每 N 轮才打一次(如 N = 2^20)，保留可观测性的同时消除 print_lock 热点。示意：

```c
unsigned long spins = 0;
while (__atomic_load_n(&online, __ATOMIC_SEQ_CST) != g_cpu_count) {
    if ((spins++ & ((1UL << 20) - 1)) == 0)
        L("CPU #%d waiting (online=%lu/%lu)", id, online, g_cpu_count);
    __asm__ volatile("pause");
}
```

或干脆删除循环内 `L()`，barrier 进入/退出各打一行即可.**留待后续优化，不在 vmm phase 范围**.

### S10.3 `kprintf` 的 `tty_ready()` 静默路径(待考察)

`kernel/printf.c:719-726`:

```c
if (online == g_cpu_count) {
    if (!tty_ready()) {
        return;
    }
}
```

所有核到屏障后，若 `tty_ready()` 返回 0，`kprintf` 直接 return:**包括 serial 输出也不打**.意图是屏障后让线程的 tty_id 与激活 tty 匹配才输出(tty 切换分流)，但副作用是屏障后任何 fault / panic 信息可能被吞，对调试是 trap.

V-2 排查 `cpu_online` 路径时一度怀疑此路径吞了 BSP 的 `Online and standing by` 输出，后由 host 端完整 `serial.log` 证实未触发(4 核 4 行齐全)，**当前不是缺陷**.但该路径的存在使屏障后调试输出依赖 `current` 与 `tty_id` 的状态，是 print/tty 子系统的待考察项，不属本文档范围。

