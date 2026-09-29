# LaOS ARM64 开发环境搭建指南

**平台**：x86_64 Linux (Debian 12 bookworm)
**目标**：交叉编译 ARM64 内核 → QEMU 模拟运行

---

## 1. 安装交叉编译工具链

```bash
sudo apt update
sudo apt install -y \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    libc6-dev-arm64-cross
```

验证：

```bash
aarch64-linux-gnu-gcc --version   # 期望: 12.2.0+
aarch64-linux-gnu-ld --version
aarch64-linux-gnu-objdump --version
```

---

## 2. 安装 QEMU (ARM64 模拟)

```bash
sudo apt install -y \
    qemu-system-arm \
    qemu-efi-aarch64
```

验证：

```bash
qemu-system-aarch64 --version     # 期望: 7.2+
qemu-system-aarch64 -M virt -cpu help 2>&1 | head -5
```

常用 CPU 型号：`cortex-a57`（保守兼容）、`cortex-a72`、`max`（QEMU 全部特性）。

### macOS Apple Silicon（LLVM/LLD）

本文前面的 GNU 交叉工具链步骤适用于 x86_64 Linux。macOS Apple Silicon
可使用 Homebrew 的 LLVM/LLD 和 QEMU 完成 ARM64 交叉构建与两种启动契约的
回归测试：

```bash
brew install llvm@22 lld@22 qemu xorriso
export PATH="$(brew --prefix llvm@22)/bin:$(brew --prefix lld@22)/bin:$(brew --prefix qemu)/bin:$PATH"

clang --version
llvm-readelf --version
qemu-system-aarch64 --version
```

然后运行：

```bash
make test-arm64-llvm
```

`llvm@22` 是 Homebrew 的 keg-only 公式；如果不修改 `PATH`，可以直接传入
工具路径：

```bash
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm \
  ARM64_READELF="$(brew --prefix llvm@22)/bin/llvm-readelf" \
  ARM64_OBJDUMP="$(brew --prefix llvm@22)/bin/llvm-objdump" \
  test-arm64-limine
```

该目标运行在 QEMU 虚拟机中，不是直接在 macOS 裸机上启动 LaOS。Limine
UEFI 测试还需要 AAVMF/edk2 ARM64 固件；脚本会自动搜索常见安装路径，也可
通过 `ARM64_UEFI_CODE` 显式指定。

### macOS 上的分层验证流程

在 Apple Silicon 上，建议将 ARM64 验证分为四层，而不是只运行一次
`test-arm64-limine`：

```text
宿主机检查/工具确认
        -> direct boot
        -> Limine UEFI/module gate
        -> 受影响子系统的专项回归
```

日常提交前的主流程是：

```bash
make clean
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm test-arm64-llvm
```

`test-arm64-llvm` 会依次验证 direct boot 和 Limine UEFI 两种启动契约，并
在两条路径共享 `obj-aarch64/` 时清理中间产物。只需要验证 Limine 时可运行：

```bash
make clean
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm test-arm64-limine
```

然后按改动范围运行最小专项目标：

```bash
# GIC、AP 上线和 SMP 停机
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm test-arm64-limine-smp-park

# SGI 与 TLB shootdown
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm test-arm64-limine-smp-tlb

# FP/SIMD 上下文
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm test-arm64-limine-fpu

# 调度、抢占和多 CPU 运行
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm test-arm64-limine-sched-stress

# 用户地址空间与任务退出
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm test-arm64-limine-multiuser

# 模块失败路径
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm test-arm64-limine-negative
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm test-arm64-limine-rollback

# virtio-blk 与 LaFS
make ARCH=aarch64 ARM64_TOOLCHAIN=llvm test-arm64-lafs
```

direct boot 和 Limine boot 不是同一个契约，不能互相替代。每个目标的成功
必须以 Makefile 或测试脚本要求的串口标志为准；QEMU 超时只有在所有要求的
标志已经出现后才算成功。

macOS Apple Silicon 上的 QEMU 通常使用 TCG 模拟。它适合验证构建、启动、
模块、异常、GIC、调度和有限的内存序路径，但不等同于真实 ARM SoC。缓存维护、
真实中断拓扑、性能、功耗、设备固件兼容性及硬件特有的弱内存行为仍需 Linux
ARM 主机、真实开发板或其他 ARM 虚拟化环境补充验证。

验证失败时先保留对应日志，不要立即覆盖：

```bash
cp build/test-arm64-serial.log /tmp/laos-test-arm64.serial.log
cp build/test-arm64-limine-serial.log /tmp/laos-test-arm64-limine.serial.log
```

至少分别确认启动标志、AP/GIC 标志、selftest 结果、模块 ABI、RCU 指令检查
和专项测试标志。IRQ 计数、启动 banner 或 QEMU 持续运行本身都不是完整的
功能证据。

---

## 3. 克隆 LaOS 代码

```bash
git clone <repo-url> LaOS
cd LaOS
```

关键文件确认：

```bash
ls kernel/arch/aarch64/                  # ARM64 移植代码（38 个文件）
ls third_party/linker-scripts/aarch64.lds # 链接脚本
```

---

## 4. 构建

### 只构建内核（最快）

```bash
make kernel ARCH=aarch64 -j$(nproc)
```

产物：`bin-aarch64/kernel`（ELF，直接被 QEMU `-kernel` 加载）。

### 构建完整 ISO（含 Limine UEFI + task.conf + user/module）

```bash
make iso-limine-arm64
```

产物：
- `bin-aarch64/kernel`（raw ELF，可直接 `-kernel` 启动）
- `build/LaOS-arm64-limine.iso`（Limine UEFI 启动镜像）

---

## 5. 运行

### 方式 A：raw kernel（推荐，2-3 秒启动）

```bash
qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu cortex-a57 \
    -m 512M \
    -kernel bin-aarch64/kernel \
    -display none \
    -serial stdio
```

预期输出：
```
================================
  LaOS — aarch64 (ARM64)  M3c
================================
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

### 方式 B：Limine UEFI ISO 启动

```bash
./run.sh
```

ISO 已采用 Limine 的 EFI boot-partition 布局，唯一菜单项会自动启动，
无需手动输入 Enter。当前 M3d 手动验收应看到：

```text
[task] queued user0 from user.elf (type=3)
[module-foo] started: count=2 tick=10
```

自动验收可执行 `./run.sh auto` 或 `make test-arm64-limine`。

### 方式 C：后台运行 + 日志

```bash
timeout 30 qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu cortex-a57 \
    -m 512M \
    -kernel bin-aarch64/kernel \
    -display none \
    -serial file:serial.log \
    2>/dev/null

grep "SYS_TEST OK" serial.log && echo "PASS" || echo "FAIL"
```

---

## 6. 自动化测试

```bash
# x86_64（KVM，~2 秒）
make test-x86_64             # 冒烟：boot + registry + cpu_alive + ipi + tlb + fpu
make test-x86_64-lafs        # virtio-blk + LaFS 真设备挂载
make test-x86_64-rollback    # 模块加载事务 checkpoint/rollback
make test-x86_64-negative    # 缺失符号模块拒绝 + selftest init fail cancel
make test-x86_64-sched-stress # 调度器压力（4 worker × 200 rounds，4 CPU）
make test-x86_64-multiuser   # 多用户进程并发退出
make test-x86_64-smp-tlb     # TLB shootdown 压测（4 CPU，50 rounds）

# aarch64（TCG，30-60 秒）
make test-arm64              # 直启冒烟
make test-arm64-limine       # Limine task.conf + 动态模块冒烟 + EL0
make test-arm64-limine-fpu   # FPU/SIMD selftest
make test-arm64-limine-sched-stress  # 调度器压力
make test-arm64-limine-multiuser     # 多用户进程
make test-arm64-lafs         # virtio-blk + LaFS 真设备挂载

# 全架构
make test-all                # x86_64 + ARM64 全链路（不含 riscv64 编译）
```

Makefile 已集成三层验证模型：

| 层级 | 架构 | 加速 | 频率 |
|---|---|---|---|
| 第一层 | x86_64 | KVM | 每次改动 |
| 第二层 | aarch64 | TCG | 每次 git commit 前 |
| 第三层 | riscv64 | TCG | 架构移植 session 中 |

---

## 7. 调试

### QEMU 异常追踪

```bash
qemu-system-aarch64 ... -d int 2>&1 | grep -E 'Taking|Exception return|ELR'
```

输出 EL 切换和异常类型（Undefined Instruction / SVC / IRQ / Data Abort）。

### 反汇编验证

```bash
# 向量表布局
aarch64-linux-gnu-objdump -d bin-aarch64/kernel | sed -n '/vector_table_el1/,/note.GNU/p' | grep '^[0-9a-f]* <.*>:'

# 特定函数
aarch64-linux-gnu-objdump -d bin-aarch64/kernel | sed -n '/<enter_el0_test>:/,/^$/p'
```

### GDB（可选）

```bash
# Terminal 1: 启动 QEMU 并等待 GDB
qemu-system-aarch64 ... -s -S

# Terminal 2: 连接 GDB
aarch64-linux-gnu-gdb bin-aarch64/kernel \
    -ex "target remote :1234" \
    -ex "b kernel_main" \
    -ex "c"
```

---

## 8. 架构相关文件速查

| 类别 | 文件 |
|---|---|
| 启动 + EL 降级 | `kernel/arch/aarch64/startup.S` |
| Limine 入口 | `kernel/arch/aarch64/limine_entry.S` |
| 异常向量表 | `kernel/arch/aarch64/entry.S` |
| 异常处理器 | `kernel/arch/aarch64/debug.c` / `debug.h` |
| 上下文切换 | `kernel/arch/aarch64/switch.S` |
| 中断控制器 | `kernel/arch/aarch64/gic.c` / `gic.h` |
| IRQ 原语 | `kernel/arch/aarch64/arch_irq.h` |
| MMU 初始化 | `kernel/arch/aarch64/vmm_arch.h` |
| TLB 操作 | `kernel/arch/aarch64/arch_tlb.h` |
| 内存屏障 | `kernel/arch/aarch64/arch_barrier.h` |
| 串口 | `kernel/arch/aarch64/serial_arch.h` / `serial.c` |
| CPU 上下文 | `kernel/arch/aarch64/cpu.h` / `cpu.c` |
| 设备树 | `kernel/arch/aarch64/fdt.h` / `fdt.c` |
| SVC 系统调用 | `kernel/arch/aarch64/syscall.c` / `syscall.h` |
| ELF 用户程序 | `kernel/arch/aarch64/user_elf_embed.h`（build 时 `xxd -i` 生成，gitignored）/ `el0_test.S` |
| 直启主函数 | `kernel/arch/aarch64/main.c` |
| Limine 主函数 | `kernel/arch/aarch64/limine_main.c` |
| ELF 加载器 | `kernel/elf_loader.c`（共享，ARM64 create_elf_process） |
| 链接脚本 | `third_party/linker-scripts/aarch64.lds` |
| Makefile | `kernel.mk`（kernel 子系统）、顶层 `Makefile` |
