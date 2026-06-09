# LaOS Mutex 设计

## 两种锁策略

LaOS 提供两套 mutex 实现，编译期通过 `CONFIG_MUTEX_HANDOFF` 选择。

### Raw Mutex (`CONFIG_MUTEX_HANDOFF = 0`)

采用经典"释放锁 -> 唤醒等待者"顺序，新来者可与被唤醒线程竞争。

```
lock (fast path):
  if cmpxchg(locked, 0 -> 1) succeeds -> return

lock (slow path):
  take waiters.lock
  if cmpxchg(locked, 0 -> 1) succeeds again -> return   // double check
  enqueue self, set BLOCKED
  release waiters.lock
  schedule()

unlock:
  take waiters.lock
  owner = NULL
  smp_mb()
  locked = 0
  if waiters exist -> __wake_up_one (delist + READY + cpu_enqueue)
  release waiters.lock
```

**特点**:
- 实现简单，仅依赖 `cmpxchg` + spinlock 保护等待队列
- 释放锁后新来者可直接抢走(barging)，唤醒的等待者可能再次竞争失败
- 吞吐量高，但等待者可能饥饿
- 被唤醒线程无独占语义：醒来后走 fast path 竞争 `locked`

### Handoff Mutex (`CONFIG_MUTEX_HANDOFF = 1`)

解锁时不释放 `locked`，直接将 owner 指针移交给队首等待者。

```
lock (fast path):
  if owner == self -> return                              // handoff 确认:锁已预留
  if owner == NULL && cmpxchg(locked, 0 -> 1) -> return   // 正常抢锁

lock (slow path, while(1)):
  if owner == self -> return                              // 可能在排队时已被 handoff
  take waiters.lock
  if owner == self -> return                              // double check
  if owner == NULL && cmpxchg(locked, 0 -> 1) -> return   // 持锁者已释放(无等待者)
  enqueue self, set BLOCKED
  release waiters.lock
  schedule()                                              // 醒来回到 while(1) 顶部
  // 注意:被 handoff 唤醒时 while(1) 顶部 owner == self 即返回

unlock:
  take waiters.lock
  if waiters exist:
    owner = first_waiter (RELEASE)                        // 移交锁所有权
    __wake_up_one (delist + READY + cpu_enqueue)
    // locked 保持 1 ---- 新来者看到 owner != NULL + locked == 1，必须排队
  else:
    owner = NULL; smp_mb(); locked = 0                    // 无等待者则释放
  release waiters.lock
```

**特点**:
- 通过 owner 指针而非 locked 计数器传递锁的所有权
- unlock 时 locked 保持 1，新来者的 fast path 检查 `owner == NULL` 短路，无法抢锁
- 被 handoff 的线程在 `while(1)` 顶部 `owner == self` 即返回，不经过 `cmpxchg`
- 严格 FIFO:队首等待者必定获得锁
- 代价:unlock 路径中 owner 原子写 + 额外的 `owner == NULL` 检查在 fast path

## 关键并发设计

### 等待队列保护

`waiters.lock` 是 spinlock，所有队列操作(入队/出队/wake)均持有此锁.spinlock 在 LaOS 上基于 `__sync_lock_test_and_set`，不可重入。

调用约定：
- `__wake_up_one(wq)`:**调用方已持有 wq->lock**，内部不做加锁
- `wake_up_one(wq)`:公开版本，内部加锁后调用 `__wake_up_one`

### Handoff 的 owner 移交时序

```
T_owner (CPU 0)                    T_waiter (CPU 1)
-----------------                  -----------------
owner = next  (RELEASE store)
__wake_up_one:
  list_del_init                    ...
  thread_set_status(READY)         ...
  cpu_enqueue                      (scheduler 选中)
  release waiters.lock
                                   enter mutex_lock_handoff:
                                     load owner (ACQUIRE) -> == self -> return [OK]
```

- RELEASE store(unlock 侧设置 owner)与 ACQUIRE load(lock 侧读取 owner)配对
- `waiters.lock` 串行化 unlock 与 lock 之间的等待队列操作，防止 lost wakeup

### Raw 路径的 barging 窗口

```
T_unlock: locked = 0               T_new: cmpxchg(0->1) 成功 -> 拿走锁
T_unlock: __wake_up_one(T_old)     T_old: 醒来，cmpxchg 失败，重新排队
```

这是 raw mutex 的预期行为 ---- 允许 barging 换取更高吞吐。

## 调度器交互与 double-enqueue 修复

### 问题发现

`CONFIG_MUTEX_HANDOFF = 1` 运行时，内核在约 20 秒后卡死。经排查，
问题不在 mutex 的 handoff 协议本身，而在调度器 `pick_next` 与 `cpu_enqueue`
的交互方式触发了 run queue 链表损坏。

### 时序差异：为何 raw 路径不受影响

raw mutex 的 unlock 在同一个 `waiters.lock` 临界区内完成三步：

```
unlock 持有 waiters.lock:
  owner = NULL
  locked = 0
  __wake_up_one -> cpu_enqueue(t)
release waiters.lock
```

而等待线程的 `schedule()` 前必须先释放 `waiters.lock`:

```
waiting thread (slow path):
  release waiters.lock
  schedule() -> __schedule() -> pick_next()
```

`waiters.lock` 充当了序列化点：唤醒者的 `cpu_enqueue` 和等待者的
`pick_next()` 不会并发操作同一个线程的 run queue 节点.**在 raw 路径下，
这一同步纯属巧合**----由 `waiters.lock` 的临界区范围恰好覆盖了整个
wake 序列----而非设计保证。

handoff 路径打破了这一巧合.handoff 接收者通过 `owner == self` 获得锁，
无需竞争 `waiters.lock`，后续的 lock/unlock 循环中 `waiters.lock` 的
临界区与 `schedule()` 的时序关系不再严格串行。

### 根因:BLOCKED 线程未摘除 run queue

核心故障路径：

```
1. 线程 B 在 pick_next 中被选为 next
   -> list_del + list_add_tail -> B 在 run queue 尾部，状态 RUNNING

2. B 进入 mutex_lock_handoff slow path
   -> thread_set_status(B, BLOCKED)   <- 状态变了，仍在 run queue 上
   -> schedule()

3. __schedule -> pick_next:
   - prev = B (BLOCKED, 仍然在 run queue 上)
   - 选择 next = C(或 idle)
   - L163: prev 状态不是 RUNNING -> 不设 READY，不从 run queue 摘除
   - switch_to(B, C)

4. 持锁者 unlock -> handoff -> __wake_up_one
   -> cpu_enqueue(B): list_add(&B->node, &rq->head)  <- B 已在 3 的 run queue 中!

5. run queue 链表损坏 -> 多次累积后 scheduler 在 list_for_each_entry 中
   死循环或访问野指针.
```

`pick_next` 中处理 prev 的逻辑：

```c
if (likely(prev != next && THREAD_RUNNING == thread_get_status(prev))) {
    thread_set_status(prev, THREAD_READY);
}
```

仅处理 RUNNING 状态的 prev.BLOCKED / SLEEPING / ZOMBIE 的 prev 留在
run queue 上不摘除。其中 ZOMBIE 由 `sched_idle_zombie` 独立摘除，
SLEEPING 由 `pick_next` 自身通过 `wakeup_ticks` 检查唤醒(不依赖外部
`cpu_enqueue`，因此不会双重入队).唯独 BLOCKED 线程存在外部
`cpu_enqueue` 与 run queue 残留节点的冲突。

### 修复

在 `pick_next` 的 else 分支增加 BLOCKED prev 的摘除：

```c
} else if (prev != next && THREAD_BLOCKED == thread_get_status(prev)) {
    list_del(&prev->node);
    atomic64_dec(&ctx->runqueue.count);
}
```

此修复将"线程被设为 BLOCKED 时必然离开 run queue"固化为调度器的显式
不变量，无论唤醒来源是 mutex，condition variable 还是其他同步原语，
均保证 `cpu_enqueue` 时线程不在 run queue 中。



## 数据竞争与内存序

| 操作 | 顺序保证 |
|---|---|
| `atomic_try_lock` (cmpxchg) | `__ATOMIC_ACQUIRE` 成功路径 + `smp_mb__after_atomic` 适配弱序架构 |
| `owner = next` (handoff) | `__ATOMIC_RELEASE` store |
| `owner == self` 检查 (lock) | `__ATOMIC_ACQUIRE` load |
| `owner = NULL` (raw unlock) | 普通写 + `smp_mb()` + `atomic_set(locked, 0)` |
| 等待队列操作 | `waiters.lock` 的 implicit acquire/release 语义 |

## 文件结构

| 文件 | 内容 |
|---|---|
| `kernel/mutex.h` | `struct mutex` / `struct wait_queue` 定义，公开 API 声明 |
| `kernel/mutex.c` | `mutex_lock` / `mutex_unlock` 分发，`mutex_lock{_raw,_handoff}`,`mutex_unlock{_raw,_handoff}`,wait_queue helpers |
| `kernel/mutex_test.c` | 多 CPU 并发测试 |
| `kernel/config.h` | `CONFIG_MUTEX_HANDOFF` 编译期开关 |
