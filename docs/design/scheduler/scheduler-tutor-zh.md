# 调度器教程

英文原文：[Scheduler tutorial](scheduler-tutor.md)。

调度器决定某 CPU 下一运行的合格线程。LaOS 跨优先级严格选择，同级采用轮转。精确运行队列约束见 [调度架构](scheduler-arch-zh.md)。

## 1. 状态与队列归属

阅读 `kernel/thread.h` 的 READY、RUNNING、BLOCKED、SLEEPING 和 ZOMBIE，再检查 `kernel/sched.c` 的 `pick_next()`。

节点已链接不表示可运行。BLOCKED 与 SLEEPING 可以留在队列，选择器按状态过滤，idle 则是每 CPU 回退任务。这也解释了互斥锁唤醒仅修改状态，而不重复插入节点。

## 2. 优先级选择

64 个桶中，0 最高，63 最低；位图标识非空桶，不免除桶内状态检查。

优先级 8、32、48 的 READY 任务同时存在时，8 被选中。其持续可运行时，让出 CPU 不保证 48 获得服务，因为选择器还与当前 RUNNING 任务比较。同级任务轮转，低优先级可以饥饿。

## 3. 分析实际切换

阅读 `__schedule()`、地址空间切换及 `kernel/arch/x86_64/switch.asm` 的 `switch_to()`。切换保存内核续执行状态和 FPU，恢复另一个栈后返回对应执行位置。

新线程的构造帧进入 `ret_from_fork`，它是首次运行跳板，不代表已有 fork syscall。

## 4. 远程唤醒

定位 `kernel/arch/x86_64/cpu.c` 的入队，再检查 `ipi_reschedule_cpu()`。发布队列先于请求。本地设置 `need_resched`，远程还发送中断。中断返回或 `preempt_enable()` 处理该标志。

CPU 放置是显式策略，不表示支持负载均衡或将运行任务迁移到空闲 CPU。

## 5. 验证优先级约束

```sh
make test-x86_64
make test-x86_64-sched-stress
```

普通 priority 检查 high、equal-A、equal-B、提升后的 low 顺序，随后验证互斥锁反转：

```text
[priority] order high=1 equal-a=2 equal-b=3 low=4
[priority] PASSED: boost=1 medium_before_unlock=0 restored=48
```

压力目标验证相邻 SMP 行为，不能替代确定性断言。捐赠阶段见 [互斥锁教程](../sync/mutex-tutor-zh.md)。

## 6. 练习：解释饥饿

设优先级 8 任务永久可运行，优先级 32 任务周期执行。说明桶 8 内轮转为何不能为桶 32 提供延迟上限，再说明将高优先级任务置为 SLEEPING 如何在不修改优先级的情况下使低优先级任务可运行。

## 7. 练习：区分队列成员关系与可运行状态

选择一个 BLOCKED 或 SLEEPING 线程，跟踪其运行队列节点、状态字段、优先级桶和位图位。说明唤醒为什么必须且只能更新一次状态和队列成员关系，以及 `need_resched` 为什么既不会自动入队，也不能证明线程已经运行。
