# LaOS 测试入口

本文是 LaOS 测试文档的统一导航页。它回答三个问题：

1. 应该运行哪个测试？
2. 具体运行方法和结果如何判断？
3. 测试涉及的配置与子系统原理在哪里说明？

本文不重复各专题文档的完整内容。新增或修改测试时，应同时更新本文和
[测试覆盖矩阵](design/testing/testing-arch.md#coverage-matrix)。

## 首先选择需要的文档

| 需求 | 阅读位置 |
| --- | --- |
| 第一次准备环境、构建并启动 QEMU | [Getting Started](getting-started.md) |
| 选择测试、运行测试、读取日志和判断 PASS/FAIL | [Testing LaOS](design/testing/testing-tutor.md) |
| 查询所有维护目标及其覆盖范围 | [Testing architecture：Coverage matrix](design/testing/testing-arch.md#coverage-matrix) |
| 根据代码改动选择最小测试集合 | [Testing architecture：Change-to-test policy](design/testing/testing-arch.md#change-to-test-policy) |
| 配置 `task.conf`、`@test` 和 selftest 参数 | [task.conf DSL](task-conf-dsl.md) |
| 编写新的内置或模块 selftest | [Testing LaOS：Add a new module selftest](design/testing/testing-tutor.md#9-add-a-new-module-selftest) 和 [task.conf DSL：添加新测试](task-conf-dsl.md#添加新测试) |
| 查询默认 CI 实际执行哪些门禁 | [Testing architecture：Hosted CI matrix](design/testing/testing-arch.md#hosted-ci-matrix) |
| 理解 QEMU 超时、串口标志和失败日志 | [Testing LaOS：Read a QEMU result critically](design/testing/testing-tutor.md#7-read-a-qemu-result-critically) |

所有当前可用命令也可以通过以下命令查看：

```sh
make help
```

## 测试源码命名

具体测试及其辅助源码统一使用 `test_<subject>` 前缀，头文件与实现文件
使用相同的基本名称，例如 `test_mutex.c` 和 `test_mutex.h`。汇编辅助文件
也遵循此规则，例如 `test_fpu_arm64_helpers.S`。

通用内核测试放在 `kernel/tests/`，架构专属测试放在
`kernel/arch/<arch>/tests/`，可加载测试载荷放在 `module/tests/`。
`selftest.c/.h` 是测试框架，`runner.c/.h` 是启动编排入口，按职责命名。

宿主机静态检查脚本使用 `check_` 前缀，QEMU 运行测试脚本使用 `test_`
前缀。测试源码名与载荷文件名分别维护；例如 `test_module_abi.c` 生成
`module_abi.mo`，后者供 `task.conf` 和启动模块列表引用。

## 宿主机检查

| 命令 | 用途 | 具体说明 |
| --- | --- | --- |
| `bash script/check_doc_links.sh` | 检查 Markdown 链接、锚点、源码路径和 Make 目标 | [Testing LaOS：Start with discovery](design/testing/testing-tutor.md#1-start-with-discovery) |
| `make test-task-conf-v1` | 检查已提交的 task.conf DSL v1 fixtures | [Testing LaOS：Start with discovery](design/testing/testing-tutor.md#1-start-with-discovery)、[task.conf DSL](task-conf-dsl.md) |

宿主机检查不启动内核，不能替代真正消费配置或执行目标路径的 QEMU
测试。

## x86_64 测试目标

| 命令 | 主要用途 | 方法与原理 |
| --- | --- | --- |
| `make test-x86_64-boot-payload` | 配置、归档、多模式、参数、多用户、负向加载与回滚聚合门禁 | [CI 覆盖](design/testing/testing-arch.md#hosted-ci-matrix) |
| `make test-x86_64-runtime-modules` | ELF 符号确认四项运行期测试位于 `.mo`，缺失载荷时不回退或误报通过 | [运行期载荷所有权](design/testing/testing-arch.md#runtime-test-payload-ownership) |
| `make test-x86_64-boot-modes` | 显式测试模式、同名 ELF 来源隔离、缺失测试配置/组件拒绝与测试切回普通 ISO 无残留 | [显式测试模式](design/boot/boot-arch-zh.md#显式测试模式) |
| `make test-x86_64-initrd` | 外置归档交接、任务入队、私有页表、多用户退出及损坏/隔离场景 | [外置 initrd 启动](design/boot/boot-arch-zh.md#外置-initrd-启动) |
| `make test-initrd` | 独立基础归档的真实解析器校验、确定性、哈希及错误输入拒绝；不验证外置归档启动 | [基础 initrd 打包](design/boot/boot-arch-zh.md#独立基础-initrd-打包) |
| `make test-boot-context` | 宿主 ASan/UBSan：文件来源隔离、CPIO 边界、独立配置、失败清理及重复执行防护 | [启动文件与配置上下文](design/boot/boot-arch.md#boot-file-sets-and-configuration-contexts) |
| `make test-x86_64` | 四核默认启动、模块、用户态和基础 SMP/selftest 门禁 | [普通架构冒烟测试](design/testing/testing-tutor.md#5-run-the-normal-architecture-smoke-tests)、[覆盖矩阵](design/testing/testing-arch.md#coverage-matrix) |
| `make test-x86_64-lafs` | PCI virtio-blk 与真实 LaFS 镜像 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[LaFS 教程](design/storage/storage-tutor.md) |
| `make test-x86_64-smp-tlb` | IPI 与重复 TLB remap 可见性 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[覆盖矩阵](design/testing/testing-arch.md#coverage-matrix) |
| `make test-x86_64-task-params` | 同一 ET_EXEC 镜像的多线程 argv/局部配置隔离与非法参数拒绝 | [模块参数契约](design/module/module-arch.md#entry-and-parameter-contracts) |
| `make test-x86_64-rollback` | 无入口模块的分配事务回滚 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[模块架构](design/module/module-arch.md) |
| `make test-x86_64-negative` | 未解析符号和 selftest 初始化失败 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[模块教程](design/module/module-tutor.md) |
| `make test-x86_64-sched-stress` | 四核调度与抢占压力 | [优先级与压力验证](design/testing/testing-tutor.md#verify-fixed-priorities-and-priority-inheritance)、[调度器架构](design/scheduler/scheduler-arch.md) |
| `make test-x86_64-rcu-stress` | 可配置 reader、宽限期和 RCU list 压力 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[RCU 教程](design/sync/rcu-tutor.md)、[RCU 架构](design/sync/rcu-arch.md) |
| `make test-x86_64-multiuser` | 多个 ring 3 用户任务的退出前完成标志 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[覆盖矩阵](design/testing/testing-arch.md#coverage-matrix) |

各目标使用的 `conf/test-x86_64-*.conf` fixture、selftest 名称及参数见
[task.conf DSL](task-conf-dsl.md)。

## ARM64 测试目标

ARM64 实现位于 `arm64` 分支。直接启动和 Limine 启动验证的契约不同，
不能互相替代。

| 命令 | 主要用途 | 方法与原理 |
| --- | --- | --- |
| `make test-arm64` | `-kernel` 直接启动、EL0/SVC 和 e1000 发现 | [普通架构冒烟测试](design/testing/testing-tutor.md#5-run-the-normal-architecture-smoke-tests)、[覆盖矩阵](design/testing/testing-arch.md#coverage-matrix) |
| `make test-arm64-limine` | 双核 UEFI/Limine、模块 ABI、用户态、remote enqueue 和 RCU publication | [普通架构冒烟测试](design/testing/testing-tutor.md#5-run-the-normal-architecture-smoke-tests)、[覆盖矩阵](design/testing/testing-arch.md#coverage-matrix) |
| `make test-arm64-limine-negative` | 未解析模块符号和 selftest 初始化失败 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[模块教程](design/module/module-tutor.md) |
| `make test-arm64-limine-rollback` | 模块缺少入口时的分配回滚 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[模块架构](design/module/module-arch.md) |
| `make test-arm64-limine-smp-park` | AP parking、GIC、online 和指定 CPU 任务标志 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[覆盖矩阵](design/testing/testing-arch.md#coverage-matrix) |
| `make test-arm64-limine-smp-tlb` | SGI 确认和重复 TLB shootdown | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[覆盖矩阵](design/testing/testing-arch.md#coverage-matrix) |
| `make test-arm64-limine-fpu` | FP/SIMD 上下文保存和恢复 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[覆盖矩阵](design/testing/testing-arch.md#coverage-matrix) |
| `make test-arm64-limine-sched-stress` | 四核调度与抢占压力 | [优先级与压力验证](design/testing/testing-tutor.md#verify-fixed-priorities-and-priority-inheritance)、[调度器架构](design/scheduler/scheduler-arch.md) |
| `make test-arm64-limine-multiuser` | 多个 EL0 用户任务的退出前完成标志 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[覆盖矩阵](design/testing/testing-arch.md#coverage-matrix) |
| `make test-arm64-lafs` | MMIO virtio-blk 和真实 LaFS 镜像 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[LaFS 教程](design/storage/storage-tutor.md) |
| `make test-arm64-lafs-negative` | 损坏 superblock 的拒绝路径 | [专项集成目标](design/testing/testing-tutor.md#6-select-focused-integration-targets)、[LaFS 架构](design/storage/storage-arch.md) |

ARM64 Limine 所需的 AAVMF 固件和首次构建步骤见
[Getting Started：ARM64](getting-started.md#arm64)。

### ARM64 主机与工具链

本分支的 ARM64 目标使用 GNU 交叉工具链，验证流程面向 Linux 主机。macOS
Apple Silicon 上的 LLVM/LLD 工作流在 `arm64` 分支维护；direct boot 与
Limine boot 必须分别验证。环境准备与硬件验证边界见
[ARM64 开发环境](process/arm64-dev-setup.md)。

## 其他入口

| 命令或方法 | 说明 | 阅读位置 |
| --- | --- | --- |
| `make test-riscv64` | 架构目录存在时执行交叉构建和启动检查，否则明确 SKIP | [覆盖矩阵](design/testing/testing-arch.md#coverage-matrix) |
| `make test-all` | 方便的聚合目标，但不包含所有专项、负向和压力测试 | [Testing LaOS：Select focused integration targets](design/testing/testing-tutor.md#6-select-focused-integration-targets) |
| `make run HEADLESS=1` | 交互式开发启动，不等价于自动化测试门禁 | [Getting Started：x86_64](getting-started.md#x86_64-first-build-and-test) |
| 早期同步 unit-style 测试 | VMA、CPIO、LaFS parser 和 block-device registry | [Testing LaOS：Understand synchronous unit-style tests](design/testing/testing-tutor.md#2-understand-synchronous-unit-style-tests) |
| `@test` 注册式 selftest | configure → prepare → start → tick → done → passed | [Testing LaOS：Configure registered selftests](design/testing/testing-tutor.md#3-configure-registered-selftests)、[task.conf DSL](task-conf-dsl.md) |

## 如何选择最小测试集合

一般顺序是：

1. 先运行宿主机静态检查。
2. 运行最直接覆盖改动契约的专项目标。
3. 再运行受影响架构的普通启动门禁。
4. 共享代码先验证 x86_64，再将提交同步到 ARM64 并运行对应 ARM64
   门禁。

具体的“改动类型 → 最小证据”表见
[Change-to-test policy](design/testing/testing-arch.md#change-to-test-policy)。
不要把编译成功、启动 banner、QEMU 存活至超时或 `make test-all`
单独当作完整测试证据。

## 日志与成功标准

运行时门禁通常检查串口中的具体行为标志和标准 selftest 结果：

```text
[selftest] 'name' PASSED
```

失败时先查看命令输出给出的 `build/*.log`。x86_64 常规串口日志通常为
`build/serial.log`；ARM64 Limine 日志通常为
`build/test-arm64-limine-serial.log`。不同专项目标可能使用独立日志名，
以 Makefile 或失败消息打印的路径为准。

正确解读超时、panic、负向测试以及串口证据的方法见
[Read a QEMU result critically](design/testing/testing-tutor.md#7-read-a-qemu-result-critically)。
