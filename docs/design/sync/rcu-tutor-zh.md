# RCU 教程

英文原文：[RCU tutorial](rcu-tutor.md)。

Read-Copy-Update（RCU，读—复制—更新）适用于读取频繁、修改较少的数据结构。读者进入开销较低的临界区；写者在回收读者可能仍在引用的对象之前等待，从而将大部分同步开销移出读路径。

LaOS 实现了用于教学的小型可抢占 RCU 模型，涵盖读者生命周期、静止状态、宽限期与延迟回收，但不具备 Linux RCU 的完整兼容性。

## RCU 解决的问题

对于共享链表，互斥锁可以保证遍历安全，但每个读者都需要获取同一个锁：

```text
读者：加锁 -> 遍历 -> 解锁
写者：加锁 -> 移除 -> 释放 -> 解锁
```

RCU 改变了对象回收规则：

```text
读者：rcu_read_lock -> 遍历 -> rcu_read_unlock

写者：从已发布的数据结构移除对象
      -> synchronize_rcu
      -> 释放
```

移除后，新读者不能再通过该结构找到对象，但已有读者可能仍持有引用。因此，写者需要等待宽限期结束后再释放对象。

## 读侧临界区

读者接口声明于 `kernel/rcu.h`：

```c
rcu_read_lock();
/* 访问受 RCU 保护的对象 */
rcu_read_unlock();
```

LaOS 在当前线程中保存嵌套计数。进入临界区时递增，退出时递减。允许临界区嵌套，只有最外层解锁才表示该线程结束读侧访问。

读侧临界区允许抢占。`rcu_read_lock()` 不禁用中断或调度抢占，读者可能在仍持有 RCU 引用时被切出。

读侧使用约束如下：

- 能否休眠取决于受保护对象的生命周期及操作约束，不能仅凭 RCU 接口名称判断。
- 不得在最外层解锁之后继续使用受 RCU 保护的指针。
- 不得释放或改作他用仍对其他读者可见的对象。
- 所有路径，包括错误路径，均须保持进入与退出配对。
- 不得在 IRQ 上半部使用当前实现的读侧接口。

RCU 保护对象生命周期，不会自动消除对象内部可变字段的数据竞争。

## 静止状态

静止状态（quiescent state）用于表明 CPU 已不再执行某个较早的读侧临界区。

LaOS 在两处记录静止状态：

1. 定时器 tick 调用 `rcu_check_quiescent_state()`。当前线程嵌套计数为零时，CPU 记录最新宽限期序号。
2. 调度器切出当前线程之前调用 `rcu_note_context_switch()`。切出的线程不在读侧临界区内时，该切换可记录为静止状态。

每个 CPU 上下文保存最近观察到的宽限期序号，写者据此检查各 CPU 的进展。

## 被抢占的读者

对于可抢占 RCU，仅检查 CPU 静止状态并不足够。线程进入读侧临界区后可能被切出，而原 CPU 随后运行其他非读者线程并报告静止状态；这时原线程仍持有旧引用。

切出这类读者之前，LaOS 将线程加入全局 `blocked_tasks` 链表：

```text
读者进入临界区
    -> 定时器抢占读者
    -> 调度器将读者记录到 blocked_tasks
    -> 原 CPU 执行其他任务
    -> 读者再次获得调度
    -> 最外层 rcu_read_unlock 移除跟踪记录
```

跟踪节点和标志位位于线程控制块中，全局链表由自旋锁保护。

## 两阶段宽限期

`kernel/rcu.c` 中的 `synchronize_rcu()` 是写侧阻塞操作：

```text
递增全局代次
        |
        v
等待所有参与的在线 CPU 观察到该代次
        |
        v
等待 blocked_tasks 为空
        |
        v
返回：相关旧读者已经完成
```

第一阶段覆盖持续运行、尚未发生上下文切换的读者。第二阶段覆盖已被抢占、不再作为任何 CPU 当前线程出现的读者。

关键不变量并不只是每个 CPU 都报告了进展。只有当前线程不在 RCU 读侧临界区时，CPU 才能报告静止状态；如果调度器切出了读者，该读者在最外层解锁前仍须保留在 `blocked_tasks` 中。只有所有参与 CPU 都跨过目标代次且没有被抢占读者继续被跟踪时，写者才能返回。

等待过程调用 `schedule()`，而非持续自旋。因此，该接口必须在可调度线程上下文中调用，且不得持有自旋锁；它不是中断上下文接口。

## 异步回收与 `call_rcu()`

LaOS 当前尚未实现 `call_rcu()`。本节说明拟采用的接口约束；以下示例为设计示例，不能直接在当前接口上编译运行。引入异步回收不应改变同步实现中的对象生命周期规则。

`call_rcu()` 将回收工作加入队列并立即返回。RCU 在一个能够覆盖可能持有已移除对象的旧读者的宽限期结束后执行回调。它不采用另一种宽限期算法，也不会缩短宽限期。

对象通常内嵌队列记录：

```c
struct item {
	int value;
	struct list_node node;
	struct rcu_head rcu;
};

static void free_item(struct rcu_head *head)
{
	struct item *item = container_of(head, struct item, rcu);
	kfree(item);
}
```

写者必须先使对象无法通过受保护结构到达，再提交回调：

```c
spin_lock(&update_lock);
list_del_rcu(&item->node);
spin_unlock(&update_lock);

call_rcu(&item->rcu, free_item);
```

其安全性依赖以下顺序：

```text
旧读者取得 item
        |
写者移除 item
        |
写者提交回调并继续执行
        |
        | 宽限期覆盖可能仍持有 item 的读者
        v
回调释放 item
```

- 移除前已取得 `item` 的读者可能继续访问它，回调必须等待该读者最外层解锁。
- 移除后才开始访问结构的读者无法再通过该结构发现 `item`。
- 新的无关读者可以与回调并发运行。RCU 等待的是可能持有旧指针的读者，而非系统中完全没有读者的时刻。

因此，`call_rcu()` 必须在移除之后调用。先提交回调会留下一个窗口，使其他读者仍能取得对象，却不一定被该回调对应的宽限期覆盖。

设计中的 `struct rcu_head` 包含回调队列链接和回调函数。内嵌记录可以避免额外分配，并允许回调通过 `container_of()` 找到所属对象。提交后须遵守以下约束：

- 所属对象及其 `rcu_head` 必须保持有效，直至回调完成。
- 同一个 `rcu_head` 不得同时重复入队。
- RCU 尚持有回调所有权时，不得将对象改作他用。
- 当前回调调用已经接管记录后，才可将该记录作为新的操作再次提交。

三个相关接口具有不同的完成条件：

| 接口 | 调用者等待的对象 | 完成含义 |
| --- | --- | --- |
| `synchronize_rcu()` | 一个宽限期 | 相关旧读者已经完成 |
| `call_rcu()` | 入队操作 | 回调将在其宽限期之后执行 |
| `rcu_barrier()` | 已提交回调 | 屏障之前提交的回调已经完成 |

`synchronize_rcu()` 不负责排空回调队列。关闭服务或卸载模块之前，应先阻止新的回调提交，再通过类似 `rcu_barrier()` 的操作等待已有回调完成，最后销毁回调代码及状态。

异步回收转移了背压，而未消除背压。缓慢或停滞的读者阻碍宽限期完成，写者却可能继续提交待回收对象。因此，实际实现需要回调积压指标、提交速率策略和停滞诊断。

LaOS 拟采用的队列、批处理和工作线程规则见 [架构说明中的异步回调方案](rcu-arch-zh.md#异步回调架构方案)。Linux 接口规范可参阅 [What is RCU?](https://docs.kernel.org/RCU/whatisRCU.html) 和 [`rcu_barrier()` 文档](https://docs.kernel.org/RCU/rcubarrier.html)。

## RCU 链表辅助接口

`kernel/list.h` 提供以下接口：

- `list_add_rcu()` 和 `list_add_tail_rcu()`；
- `list_del_rcu()`；
- `list_for_each_rcu()` 和 `list_for_each_entry_rcu()`。

基本用法如下：

```c
/* 写者之间仍需串行化。 */
spin_lock(&update_lock);
new_item->value = value;
list_add_rcu(&new_item->node, &items);
spin_unlock(&update_lock);

rcu_read_lock();
list_for_each_entry_rcu(item, &items, node) {
    consume(item->value);
}
rcu_read_unlock();

spin_lock(&update_lock);
list_del_rcu(&old_item->node);
spin_unlock(&update_lock);
synchronize_rcu();
kfree(old_item);
```

`list_del_rcu()` 在宽限期完成之前保留已移除节点的链接，因为停留在该节点上的读者可能仍需通过 `next` 继续遍历。

这些辅助接口只支持前向遍历。`prev` 仍是写者维护的链接，因此反向遍历不属于 RCU 契约。写者仍须与其他写者串行化；`list_first_entry_rcu()` 要求链表非空，LaOS 当前尚未提供返回空指针的首元素辅助接口。

写者互斥与读者保护是独立要求。两个写者不能在缺少写锁或等效单写者规则的情况下，同时修改普通链表链接。

## 内存顺序约束

发布对象需要满足两项条件：

1. 发布使对象可达的链接之前，完成对象初始化。
2. 读者观察到该链接时，也能够观察到已初始化字段。

RCU 链表辅助接口使用 release-store 发布前向链接，使用 acquire-load 遍历。编译器原子操作在各受支持架构上保持相同的源码级约束：x86_64 通常不需要额外指令，ARM64 则生成 `STLR`、`LDAR` 或等效序列。

对应的实现与验证关系如下：

- RCU 宽限期核心位于共享代码中。
- 有界 `rcu_publish` 自测试在 x86_64 和 ARM64 上验证发布、观察、移除及宽限期后的复用。
- ARM64 构建检查还核对生成的 acquire/release 指令，因为单次 QEMU 运行不足以充分验证内存顺序。

等待旧读者完成与发布新数据属于相关但不同的正确性要求。

## 配置与实验

`kernel/config.h` 中的 `CONFIG_RCU` 控制该实现。禁用后，读侧、写侧及集成钩子编译为无操作桩函数，可用于隔离调度问题，但也同时取消了 RCU 生命周期保证。调用者不得假定宽限期已经发生并据此回收共享对象。

`CONFIG_RCU_DEBUG` 输出跟踪链表状态变化，日志量较大。

内置 `rcu_stress` 自测试接受 `rounds`、`readers` 和 `timeout_ticks`。各读者在读侧临界区中主动调用 `schedule_timeout(1)`，确定性地覆盖可抢占读者的 `blocked_tasks` 路径，无须依赖定时器恰好在忙循环中触发。

写者先执行一个普通宽限期，再逐轮执行发布、观察、移除、宽限期等待和释放。x86_64 专项测试命令如下：

```sh
make test-x86_64-rcu-stress
```

较短的 `rcu_publish` 测试使用一个静态节点，检查宽限期后的复用，但不执行堆回收。普通 x86_64 和 ARM64 Limine 测试运行该短测试；当前没有 ARM64 RCU 压力测试目标，因此包含被抢占读者、分配和释放的专项负载目前仅由 x86_64 专项测试验证。ARM64 普通测试还会检查生成的 acquire/release 指令形式。

该压力测试覆盖预期的多 CPU 路径，不能证明所有编译器变换、弱内存顺序、多写者行为、CPU 热插拔或任意硬件停滞情形均正确。

## 尚未提供的能力

- `call_rcu()` 及异步回调工作线程；
- 回收回调批处理；
- 加速宽限期；
- 停滞检测与诊断；
- CPU 热插拔集成；
- 中断和 NMI 读侧变体；
- RCU 链表辅助接口之外的通用 release/acquire API；
- 面向外部使用者的稳定 RCU ABI。

当前写者同步等待，缓慢或停滞的读者会直接延迟写者。

## 扩展练习

1. 增加解锁计数下溢及线程退出时嵌套计数非零的断言。
2. 增加有界停滞诊断，输出滞后的 CPU 和被跟踪线程。
3. 将已有链表发布语义提炼为架构正确的 `smp_store_release` 和 `smp_load_acquire` 通用接口，并在 ARM64 上运行链表测试。
4. 串行化并发宽限期写者，或明确设计其并发规则。
5. 增加 `call_rcu()` 回调队列与工作线程，并保持回调顺序和关闭语义。
