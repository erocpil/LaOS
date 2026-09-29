# 调度器架构

英文原文：[Scheduler architecture](scheduler-arch.md)。源码阅读步骤见 [调度教程](scheduler-tutor-zh.md)，与中断及用户执行的联系见 [第 4 章](../../book/04-scheduler-zh.md)。

## 适用范围

LaOS 采用每 CPU 固定优先级调度器，提供 64 个静态优先级（0 最高、63 最低）、默认优先级 32、跨优先级严格选择、同级可运行线程轮转、显式 CPU 亲和性、远程唤醒与重调度 IPI，以及单跳互斥锁优先级继承。

当前没有动态负载均衡、按时间衰减优先级、截止时间调度、CFS 式比例公平或饥饿预防。

## 源码分工

| 文件 | 职责 |
| --- | --- |
| `kernel/thread.h`、`kernel/thread.c` | 优先级状态与修改接口 |
| `kernel/sched.c` | 可运行任务选择及切换策略 |
| `kernel/arch/*/cpu.c` | 每 CPU 运行队列入队与出队 |
| `kernel/arch/*/ipi.c` | 本地与远程重调度请求 |
| `kernel/mutex.c` | 优先级等待队列与捐赠 |
| `kernel/test_priority.c` | 确定性执行顺序及反转验证 |
| `kernel/arch/x86_64/switch.asm` | x86_64 寄存器与 FPU 切换 |
| `kernel/arch/aarch64/switch.S` | ARM64 寄存器与 FP/SIMD 切换 |

ARM64 实现位于 `arm64` 分支。

## 优先级状态

| `struct thread` 字段 | 含义 |
| --- | --- |
| `base_priority` | `thread_set_priority()` 指定的基础优先级 |
| `priority` | 决定运行队列桶的有效优先级 |
| `pi_donations[64]` | 各优先级的活动互斥锁捐赠计数 |
| `pi_lock` | 串行化基础及有效优先级变化 |

有效优先级计算为：

```text
min(基础优先级, 所有活动捐赠中的最小数值)
```

捐赠计数允许线程持有多个存在竞争的互斥锁。释放一个锁仅撤销该锁的贡献，保留其他捐赠。

动态线程及架构拥有的静态 TCB 均必须调用 `thread_priority_init()`。`thread_set_priority()` 成功返回 0，非法参数返回 -1，修改阻塞等待者返回 -2。拒绝修改的原因是其有效优先级已计入锁所有者的捐赠，而当前尚未实现两者的原子更新。

## 每 CPU 运行队列

每 CPU 维护 64 个链表头、非空位图、原子节点计数与自旋锁。位图置位仅表示链表存在节点，不表示存在可运行任务。RUNNING、BLOCKED 和 SLEEPING 均保留在队列，选择时仍须检查状态。

维护的不变量为：

1. 每个非 idle 活线程最多链接一次。
2. 节点位于 `heads[thread->priority]`。
3. 桶为空当且仅当其位图位清零。
4. `count` 统计已链接节点，不统计 READY 任务数量。
5. idle 是 `cpu_context` 的回退任务，不是普通队列节点。
6. zombie 先离开运行队列，再将节点用于 zombie 队列。

ARM64 直接启动与 Limine 静态 idle/boot TCB 遵循同样的初始化规则。idle 不插入桶 0，否则会成为最高优先级候选。

运行队列不变量也可以表示为状态转换契约：

```text
新建/自链接 -> 入队：READY + 插入优先级桶
READY       -> 选择：RUNNING
RUNNING     -> 阻塞/休眠：状态改变，节点仍保留
BLOCKED/SLEEPING -> 唤醒/到期：READY + 原桶或新桶
zombie      -> 节点复用前先出队
```

保留链接节点是有意设计，但这意味着队列成员关系与可运行状态是两个不同谓词。任何新的唤醒路径都必须对线程状态和队列节点各更新一次，不能重复操作。

## 选择算法

`pick_next()` 在本地中断关闭、持有运行队列锁时执行：

1. 复制非空位图。
2. 用 `ctz` 定位最低置位。
3. 查找该桶中的 READY 或已到期 SLEEPING 任务。
4. 无可运行任务时，仅清除局部位图副本的该位，继续查找。
5. 将候选与当前非 idle RUNNING 线程比较。
6. 无候选或候选优先级严格更低时，保留当前任务。
7. 否则选择候选，并将其节点移到桶尾。

当前线程是 RUNNING 而非 READY，因此必须单独比较，否则低优先级 READY 任务可能替换高优先级当前任务。

同级时，另一个 READY 任务胜出并移至尾部，定时器抢占形成轮转。选中当前线程时，`__schedule()` 直接返回，不人为执行切换或 RCU 切换钩子。

严格优先级允许持续运行的优先级 8 任务使优先级 32 任务饥饿。这是策略结果。旧无限循环互斥锁线程因此仅由 `CONFIG_MUTEX_STRESS` 显式启用，默认不为每 CPU 创建永久运行的默认优先级压力线程。

选择循环不提供跨优先级公平性。轮转只发生在选定最高有效优先级桶内部，不能抵消持续可运行的高优先级任务造成的低优先级饥饿。

## 修改已入队线程的优先级

仅修改 `thread->priority` 会导致 TCB 与所在桶不一致。`thread_apply_effective_priority()` 关闭本地中断并持有 PI 锁，获取目标运行队列锁，移除旧桶节点及维护位图，修改有效优先级，插入新桶尾并设置位图，最后释放队列并请求目标重调度。

已初始化但未入队的节点自链接，只更新 TCB，后续入队使用新值。

```text
thread.pi_lock -> 目标 runqueue.lock
```

调度器只获取运行队列锁，不获取 PI 锁。

## 唤醒与重调度顺序

入队将线程置为 READY，在目标队列锁保护下插入，再调用 `ipi_reschedule_cpu()`。两种架构的本地请求均直接设置 `need_resched`，不向自身发送中断。远程请求先以 release 语义写入标志，再发送中断：

- x86_64 当前广播，尚未将逻辑 CPU 编号反向映射至任意 APIC ID。
- ARM64 针对 QEMU `virt` 亲和布局发送定向 SGI 3。

中断返回和 `preempt_enable()` 以 acquire 语义读取标志，因此本地入队不必等待无关定时器 tick。

远程唤醒的发布顺序如下：

```text
目标线程状态 = READY
在目标运行队列锁下入队
以 release 语义发布 target->need_resched
发送本地标志或远程 IPI/SGI
目标返回路径以 acquire 语义观察 need_resched
```

队列锁保护成员关系，`need_resched` 只请求调度点。两者均不能单独证明目标线程已经运行。

## 上下文布局的构建依赖

架构切换使用 `struct thread` 的生成偏移，尤其是 `THREAD_FPU_STATE`。扩展 TCB 后不重编汇编，可能按旧偏移写入并破坏线程名称或队列节点。

`script/gen_offsets.sh` 仅在内容改变时更新文件。`kernel.mk` 显式声明 x86_64 NASM switch/syscall 对 `asm_offsets_nasm.inc` 的依赖；ARM64 `.S` 使用编译器生成的 `.d`，两条启动构建路径均先生成偏移。增量构建正确性属于 TCB ABI 要求。

## 测试约束

```text
@test priority timeout_ticks=500
```

顺序阶段入队 low（48）、equal A/B（32）和 high（8），要求 high、A、B 依次运行，低优先级不能提前运行。随后将已链接 low 提升到 8，要求其第四个运行，同时验证桶迁移。

反转阶段由 low 获取锁并将基础优先级降至 48；medium（32）与 high（8）变为可运行，high 阻塞并捐赠 8。low 必须先于 medium 执行，观察提升、解锁并恢复 48，随后 high 获取及释放锁，medium 最终运行。

```text
[priority] order high=1 equal-a=2 equal-b=3 low=4
[priority] PASSED: boost=1 medium_before_unlock=0 restored=48
[selftest] 'priority' PASSED
```

```sh
make test-x86_64
make test-x86_64-sched-stress
make test-arm64
make test-arm64-limine
make test-arm64-limine-sched-stress
```

ARM64 命令在对应分支执行。普通 x86_64 和 ARM64 Limine 目标要求 priority 标志；直接 ARM64 验证静态 TCB 与 idle 回退构造；压力目标验证相关 SMP 路径。

验证结果应按结论解释：`priority` 证明确定性选择和已测试的捐赠路径；`remote_enqueue` 证明远程唤醒请求得到处理；`sched_stress` 覆盖重复状态转换。它们均不证明无饥饿、CPU 热插拔或任意亲和性变化。

## 限制与后续扩展

优先级仅由内核接口配置，任务配置与用户 syscall 不分配优先级。没有老化、带宽控制、饥饿检测、负载均衡或 CPU 热插拔。位图定位为 O(1)，桶内状态筛选仍为线性。继承仅单跳，不支持 BLOCKED 基础优先级修改。

后续可先增加有界饥饿或延迟观测，或显式任务优先级配置。传递性继承需要等待依赖链、环处理与独立嵌套锁测试。
