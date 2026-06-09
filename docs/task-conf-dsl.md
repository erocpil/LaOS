# task.conf DSL

`task.conf` 是 LaOS 的 **boot-time orchestration DSL**：它在 boot module
阶段描述内核应加载哪些模块、在哪个 CPU 上启动哪些任务、启用哪些 selftest。

当前目标不是把它做成复杂语言，而是先把已经存在的格式规范化，保留向后兼容空间，
再为后续测试模块化和跨架构验证对齐预留 directive 语义。

---

## 设计边界

- `task.conf` parser 是启动核心能力，静态链接进内核。
- 测试用例可以逐步做成 `.mo` 模块，由 `task.conf` 按需声明并加载。
- parser 本身不建议做成 `.mo`：
  - parser 早于普通任务和测试模块，是 boot chain 的基础；
  - 动态 parser 会引入"先解析配置才能知道加载 parser"的自举问题；
  - 还需要额外 ABI、版本协商、错误报告和恢复路径，当前没有足够收益。

---

## 文件格式

### 版本声明（必须第一行）

```text
@version 1
```

### 指令行（`@` 开头）

```text
@test <name> [key=value ...]
```

未知指令视为配置错误（不静默忽略）。

### 模块缺失策略

```text
@module_missing skip|panic
```

控制内核遇到未找到的模块（如架构不匹配的 `.mo`）时的行为：
- `skip`（默认）：跳过未知模块，继续启动
- `panic`：立即 panic

示例：
```text
@module_missing skip
@module_missing panic
```

### 任务行

```
CPU<TAB>MODULE[:ENTRY]<TAB>TYPE<TAB>MAGIC[<TAB>ARGS...]
```

### 注释与空白

- 空行忽略。
- `#` 开头为注释。
- token 由空格或 tab 分隔。

---

## 任务行参考

| 字段 | 含义 | 示例 |
|---|---|---|
| `CPU` | 目标逻辑 CPU id，十进制（0-based） | `0`, `1`, `2`, `3` |
| `MODULE` | boot module 文件名，可选入口别名 | `module_foo.mo:foo`, `user.elf:user0` |
| `ENTRY` | `:` 后的任务名（可选） | `foo` |
| `TYPE` | 任务类型（见下表） | `1`, `2`, `3` |
| `MAGIC` | 十六进制魔数，支持 `0x` 前缀 | `0xa`, `0xb`, `0xc` |
| `ARGS` | kv 参数和/或位置参数，空格分隔 | `count=2 tick=10` |

### TYPE 枚举

| 值 | 含义 | 加载方式 |
|---|---|---|
| `1` | kthread module（`.mo`） | ELF rel 重定位 → `main(argc,argv)` |
| `2` | 独立 ELF | ELF exec 加载 → entry |
| `3` | 用户态 ELF | ELF exec → EL0 用户线程 |

### ARGS 规则

- `key=value` → Linux 风格 kv 参数，内核匹配模块 `__laos_params` 段并写入变量
- 裸 token → 位置参数，传入 `main(argc, argv)`
- kv 与位置参数可混合，`=` 有无自动区分

### 示例

```text
# CPU MODULE              TYPE MAGIC [ARGS...]
0     module_foo.mo:foo   1    0xa   count=2 tick=10
0     module_abi.mo:abi   1    0xb
0     user.elf:user0      3    0xc
2     e1000.mo:e1000      1    0xa   g_idle_mode=1
```

---

## `@test` 指令参考

```text
@test <name> [key=value ...]
```

### 支持的 kv 参数

| 参数 | 含义 | 适用测试 |
|---|---|---|
| `module=<name>` | 指定 `.mo` 载荷文件名 | 所有模块测试 |
| `rounds=<n>` | 测试轮数 | `ipi_delivery`、`smp_tlb_remap`、`fpu_context`、`rcu_publish`、`rcu_stress` |
| `readers=<n>` | RCU reader 数（不超过 online CPU 数） | `rcu_stress` |
| `timeout_ticks=<n>` | 超时（tick 数） | `cpu_alive`、`ipi_delivery`、`priority`、`remote_enqueue`、`rcu_publish`、`rcu_stress` |

内置测试（无 `.mo` 载荷）不需要 `module=` 参数：

```text
@test registry
```

### 加载流程

```
task.conf parse → directive records → selftest_load_payloads()
  → selftest_init() → selftest_register()
  → selftest_apply_all() → configure() + prepare()
  → selftest_run() → start() → tick() → done()
```

---

## 预定义模块目录

### selftest 载荷（`@test` 引用）

| 名称 | 文件 | 架构 | 说明 |
|---|---|---|---|
| `cpu_alive` | `test_cpu_alive.mo` | 通用 | CPU alive 探测：所有 online AP 回应一次 IPI |
| `ipi_delivery` | `test_ipi_delivery.mo` | 通用 | IPI 递送压测：多轮 broadcast + 计数校验 |
| `priority` | 内建 | 通用 | 固定优先级、同级 FIFO 轮转、已入队迁移与 mutex 单跳优先级继承 |
| `remote_enqueue` | 内建 | 通用 | BSP 在 AP online 后动态入队 worker，校验 reschedule IPI 与目标 CPU 执行 |
| `rcu_publish` | 内建 | 通用 | 有界跨 CPU RCU 发布/摘链/宽限期与最终空链表校验 |
| `rcu_stress` | 内建 | 通用 | 可配置多 reader 压力测试，确定性覆盖临界区内调度、宽限期及发布/回收 |
| `smp_tlb_remap` | `test_tlb.mo` | dual-arch | TLB shootdown remap 可见性 |
| `fpu_context` | `test_fpu_context.mo` | x86_64 | FPU 上下文保存/恢复（x87 + XMM） |
| `fpu_arm64` | `test_fpu_arm64.mo` | ARM64 | NEON SIMD 上下文保存/恢复（Q0-Q31） |
| `sched_stress` | `test_sched_stress.mo` | 通用 | 调度器压力测试（N worker × M rounds 多核） |
| `init_fail` | `test_init_fail.mo` | 通用 | selftest_init 返回 -1，验证 cancel 回滚 |
| `registry` | （内置） | 通用 | 模块注册表三态自检：reserve/cancel/full |

### kthread 模块（任务行 `TYPE=1`）

| 文件 | 架构 | 说明 |
|---|---|---|
| `module_foo.mo` | 通用 | 基础 smoke：打印 `count`/`tick`，超时后 reschedule |
| `module_abi.mo` | 通用 | 重定位 + data + bss 回归测试 |
| `module_smp_probe.mo` | ARM64 | 一次性 SMP 多核探针：记录所在 CPU 后返回，不占用 AP |
| `module_bad.mo` | 通用 | 未解析符号（负向测试用） |
| `module_no_entry.mo` | 通用 | 无 main/_start 入口（回滚测试用） |
| `e1000.mo` | 通用 | Intel E1000 网卡驱动（x86_64: 可加载模块，运行时注册 IDT ISR；ARM64: 内建驱动） |

### 用户态 / 独立 ELF（`TYPE=2` 或 `3`）

| 文件 | 架构 | 说明 |
|---|---|---|
| `user.elf` | 通用 | EL0 用户态程序（crt0 + SVC syscall） |
| `thread_bar.elf` | x86_64 | 独立 ELF 线程 |

---

## 添加新测试

### 模块测试（需要 `.mo` 载荷）

1. 创建 `module/test_<name>.c`，实现 `selftest_init()` 调用 `selftest_register()`
2. `module/Makefile` 添加构建规则，注意 `$(SELFTEST_DEPS)` 依赖
3. `conf/task.conf`（或对应架构 conf）加 `@test <name> module=test_<name>.mo`
4. 门禁脚本加 `grep "[selftest] '<name>' PASSED"`

### 内置测试（无 `.mo` 载荷）

1. 创建 `kernel/test_<name>.c`，实现 `struct selftest` + `selftest_register()`
2. 启动路径调用初始化函数注册
3. `conf/task.conf` 加 `@test <name>`（无需 `module=`）
4. 门禁同上

---

## 各 conf 当前启用的功能

| conf 文件 | `@test` 指令 | 任务 |
|---|---|---|
| `task.conf` | registry, priority, remote_enqueue, rcu_publish, cpu_alive, ipi_delivery, smp_tlb_remap, fpu_context | user.elf×6, module_foo.mo×4, thread_bar.elf×1, e1000.mo×1 |
| `task-x86_64-tlb.conf` | registry, cpu_alive, ipi_delivery, smp_tlb_remap | user.elf×6 |
| `task-x86_64-rollback.conf` | registry | module_no_entry.mo×1, module_foo.mo×1, user.elf×1 |
| `task-x86_64-negative.conf` | init_fail | module_bad.mo×1, module_foo.mo×1, user.elf×1 |
| `task-x86_64-stress.conf` | registry, cpu_alive, ipi_delivery, sched_stress | module_foo.mo×1, user.elf×1 |
| `task-x86_64-rcu-stress.conf` | rcu_stress | user.elf×1 |
| `task-x86_64-multiuser.conf` | （无） | user.elf×3, module_foo.mo×1 |
| `task-arm64.conf` | （无） | module_foo.mo×1, module_abi.mo×1, user.elf×1, module_smp_probe.mo×3 |
| `task-arm64-tlb.conf` | registry, cpu_alive, ipi_delivery, smp_tlb_remap | module_foo.mo×1, module_abi.mo×1, user.elf×1 |
| `task-arm64-negative.conf` | init_fail | module_bad.mo×1, module_foo.mo×1, module_abi.mo×1, user.elf×1 |
| `task-arm64-rollback.conf` | （无） | module_no_entry.mo×1, module_foo.mo×1, module_abi.mo×1, user.elf×1 |

---

## TTY 控制台

LaOS 支持多路虚拟终端（10 路，索引 0-9）。按键 `0`-`9` 可在终端间切换。

| TTY | 内容 | 刷新策略 |
|-----|------|---------|
| 0 | `stats_sys()` — 系统概览（uptime、CPU、内存、任务清单） | 切换时渲染一次，50 tick 低功耗轮询 |
| 6 | `stats_conf()` — **任务配置全景视图**：@version、@module_missing 策略、@test 指令清单、按 CPU 分组的任务表（含位置参数和 kv 参数） | 切换时渲染一次，50 tick 低功耗轮询 |
| 7 | `stats_net()` — e1000 网卡统计 | 每 TIMER_HZ/2 刷新 |
| 8 | `stats_rcu()` — RCU 统计 | 每 TIMER_HZ/2 刷新 |
| 9 | `stats_cpu()` — CPU 占用率（per-core + per-thread %CPU） | 每 TIMER_HZ 刷新 |

TTY 6 是专门展示 `task.conf` 解析结果的监控面板，由 `kernel/stats.c:stats_conf()` 渲染、
`kernel/monitor.c` 的 monitor 线程驱动。解析结果遍历 `task_conf` 全局状态中的指令链表
（`task_conf_get_directives()`）和任务链表，动态渲染而非回显原始文件。
