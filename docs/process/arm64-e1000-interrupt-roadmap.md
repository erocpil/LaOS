# ARM64 e1000 中断驱动路线图

当前 ARM64 已启用 GICv3 CPU interface 和 100 Hz timer IRQ，调度器能够在
EL1/EL0 IRQ 返回路径上执行延迟抢占。e1000 RX 已由 GIC INTx 唤醒专用
`net-rx` worker，不再依赖无条件轮询。

## 第二阶段任务

1. 清理调度器与 CPU 队列接口的编译警告。
   - 消除 `cpu_enqueue`、`cpu_enqueue_tail`、
     `cpu_dequeue_zombie_tail` 和 CPU ID 接口的隐式声明。
   - 公共接口必须同时兼容 x86_64 和 aarch64，不能改变现有 x86 ABI。
   - 特别检查 ARM64 上返回指针被隐式推断为 `int` 的截断风险。
2. 从 DTB PCI host bridge 的 `interrupt-map` 解析 PCI INTx 到 GIC SPI 的路由，
   避免将 QEMU 当前使用的 IRQ 号硬编码为平台约定。
3. 完善目标 GIC SPI 的 Group、优先级、触发类型和 enable 配置，并验证
   IAR -> handler -> EOIR -> DIR 的处理顺序。
4. 启用 e1000 RX 中断：初始化后清 `ICR`，设置 `IMS`，先启用 RXT0、
   RXDMT0 和 RXO，并在 ISR 中读取 `ICR` 确认中断来源。
5. 保持 ISR 轻量：只回收 RX 描述符、记录事件或唤醒网络线程；ARP/ICMP
   等较长处理放到线程上下文。
6. 中断收包稳定后移除测试线程中的 RX polling，避免 ISR 与 polling 同时
   消费描述符环。
7. 回归验证：timer 持续抢占、EL0 ELF 正常运行和退出、宿主机持续 ping、
   ARP/ICMP 收发，以及连续流量下无丢中断和调度异常。

## x86_64 功能移植顺序

1. 完成 e1000 的 GIC/PCI INTx 中断收包路径。
2. 支持 ARM64 Limine 启动、module request 和 `task.conf` 任务链。
3. 将 ARM64 用户 ELF 路径并入公共进程地址空间和 ELF loader。
4. 通过 PSCI 或 Limine SMP 启动辅助 CPU，初始化 per-CPU context。
5. 实现 GIC SGI、跨核 reschedule 和 TLB shootdown。
6. 在多核 ARM64 上验证调度器、mutex、rwlock 和 RCU 压力场景。
7. 增加 ARM64 独立异常栈、栈回溯和完整故障诊断。
8. 在需要时实现 MSI/MSI-X；GICv3 平台对应 ITS/LPI 路径。
9. 清除公共代码残留的 x86 数据模型命名和兼容桩。

## 架构抽象迁移规则

- 公共代码使用 `arch_*` 语义接口，不直接使用 LAPIC、APIC ID、MSR、
  CR3、RFLAGS、`cli` 或 `sti` 等 x86 名称。
- `arch/x86_64` 和 `arch/aarch64` 分别实现相同语义；不存在对应硬件时，
  不以伪造 MSR/LAPIC 的方式掩盖调用，而应删除无意义调用或提供新的语义接口。
- `pml4_phys`、`active_pml4_phys` 以及 VMM 参数中的 `pml4` 属于数据模型泄漏，
  后续统一迁移为 `page_table_root_phys`、`active_page_table_root_phys` 和
  `page_table_root`。该重构应与公共 ELF loader 合并一起进行，避免同时改变
  线程 ABI、汇编偏移和页表遍历逻辑。

## 当前验收基线

- `make kernel ARCH=aarch64 TOOLCHAIN=aarch64-linux-gnu -j` 构建成功。
- QEMU `virt,gic-version=3,highmem=off` 下 timer IRQ 已连续运行数千 tick。
- tap0 环境下 e1000 polling 能完成 ARP 和 ICMP 收发。

## 当前进度

- PCI INTx 路由已按 `interrupt-map-mask` 的 masked BDF + pin 匹配；确认
  QEMU virt 上 00:01.0 INTA 经 slot swizzle 路由到 GIC ID 36。
- GIC SPI 已配置为 Group 1、level-triggered、优先级 0xA0，并路由到 boot CPU。
- e1000 已开启 RXT0/RXDMT0/RXO，最小 ISR 能读取并统计 `ICR`；tap0 ping
  验证出现连续 `ICR=0x80/0x83`。
- ISR 通过 release/acquire 原子事件通知测试/网络线程，RX 描述符和协议处理
  均在线程上下文完成；每轮无条件 MMIO polling 已移除。专用 `net-rx`
  worker 在无事件时保持 BLOCKED，由 ISR 改回 READY 并请求重新调度。
- 已开始 Limine 迁移：新增 bootloader-independent `boot_info`，将 memory map、
  module list、DTB 和 HHDM 与 Limine 协议结构隔离；x86 启动路径先作为适配器
  和回归基线，ARM64 direct boot 暂时保持不变。
