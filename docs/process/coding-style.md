# LaOS Coding Style

本文档定义三档注释风格与三条一致规则。新代码按此标准，旧代码渐进对齐。

---

## 档 1:文件 / 子系统顶注块 - `/* ... */`

**用于**:`.h` 文件头部的设计说明，调用方契约，不变量，错误处理策略。

**规则**:长段散文，不用 `/**`(doxygen 风格)，因为顶注块是给人读的设计说明，不是挂在某个符号上的元数据。

**范例**(`kernel/pmm.h` 顶部):

```c
/*
 * Physical Memory Manager (PMM)
 *
 * bitmap-based first-fit 物理页分配器，单页粒度 PAGE_SIZE = 4KB.
 *
 * 调用方契约
 * ----------
 * - pmm_init 在 boot 阶段单线程调用，**不加锁**，传 limine memmap response.
 * - 所有运行期公共函数(pmm_alloc / pmm_free / ...)由 pmm 内部加锁，
 *   调用方**不需要持 pinfo.pmm_lock**.
 *
 * 不变量
 * ------
 * - usable - freed = used(推导，不直接存)
 * - 物理 [0, PMM_LOWMEM_LIMIT=0x100000) 范围永不分配.
 *
 * 错误处理
 * --------
 * - pmm_alloc / pmm_alloc_pages 返回 NULL，不 panic.
 * - pmm_free 单页 double-free -> panic;多页 -> 单页跳过 + L() 警告.
 */
```

**间隔三空行再接 `#ifndef` 或 `#include`(视觉上将顶注与实质性代码分离).**

---

## 档 2:函数 / 结构上方契约块 - `/** ... */`

**用于**:单个函数，单个 struct 的非平凡接口契约。简单 getter / setter 跳过。

**规则**:
- 用 `/**` 作为视觉锚点(IDE / clangd 能定位为"这是该符号的正式说明").
- **不写 `@param`，`@return`，`@brief`**: LaOS 不跑 doxygen，加 tag 是冗余。
- 仅写**调用方需要知道的事**(跨上下文约束，锁序要求，正确性前提)，不写"该函数做了 X"这种可从函数名直接读出的废话。

**范例**(`kernel/rcu.h` 风格):

```c
/** Reader API
 *
 * rcu_read_lock / rcu_read_unlock 必须严格配对.允许嵌套(nesting++/--).
 * 方案 B 不关抢占，临界区内可被 schedule 抢占，
 * __schedule 路径会调用 rcu_note_context_switch 把线程登记到 blocked_tasks.
 */
static inline void rcu_read_lock(void);
static inline void rcu_read_unlock(void);
```

```c
/** synchronize_rcu - 阻塞调用方直到当前所有 reader 退出临界区
 *
 * 两阶段实现:
 *    阶段一:atomic_inc(gp_seq) 后自旋等待所有 CPU 观察到新 gp_seq
 *    阶段二:等待 blocked_tasks 链表清空
 *
 * 调用要求:不可在 RCU 读侧临界区内调用(自死锁).
 */
void synchronize_rcu(void);
```

含参数的函数范例：

```c
/** vmm_destroy_level - 递归销毁页表结构并释放物理页
 *
 * table_phys:页表物理地址，调用方保证其 phys_to_virt 映射存在.
 * level:当前层级编号.4=PML4，3=PDPT，2=PD，1=PT.
 *   level 从 4 向下递归至 1，逐层解析 entry -> 递归下一级 -> 释放本级物理页.
 *
 * 返回值:无.销毁后页表树不可用，相关 TLB 项需调用方手动刷新.
 */
```

格式规则：
- 参数名后接中文冒号 `:`(不是英文 colon)，再接参数说明。
- 参数说明可以跨行，续行缩进两空格。
- `返回值:` 作为独立段落，后接说明文字。
- 一参数一行的方式保留了可读性，同时避免 `@param` 标签带来的 doxygen 冗余。

结构体范例：

```c
/** rcu_global_state_t - RCU 全局状态(方案 B:可抢占 RCU)
 *
 * - gp_seq: 当前宽限期序号，atomic 递增.
 * - blocked_tasks: 被抢占且仍位于 RCU 临界区内的线程链表.
 * - blocked_lock: 保护 blocked_tasks 的自旋锁.
 *
 * 字段访问规则:gp_seq 无锁原子读;blocked_tasks 的增/删/查必须持锁.
 */
typedef struct {
    atomic_t gp_seq;
    spinlock_t blocked_lock;
    struct list_node blocked_tasks;
} rcu_global_state_t;
```

---

## 档 3:实现内解释 - `//`

**用于**:函数体内算法步骤，pitfall 提示，锁序约束，临时说明。

**规则**:
- 回答"**为什么**"，不复读"**是什么**".
- 便于 diff:单行增减不波及其他内容。

**范例**(反例在前，正例在后):

```
// 反例 - 复读代码，不如不写
// 增加计数
counter++;

// 正例 - 解释为什么这么写
// 不能直接用 memcpy:src/dst 在回收测试中可能重叠
```

```
// 正例 - pitfall 提示
// 持锁期间不可调用 schedule:__schedule 路径会取 rcu_state.blocked_lock
// 触发锁序违反(先 project_lock 后 blocked_lock)
```

---

## 三条一致规则

### 规则 A:中文散文 + English 接口 token

字段名，函数名，宏，CPU 寄存器名，数据结构名称保持 English.解释说明文字用中文。

```
// [OK] 正确
// struct thread 的 rcu_nesting 字段是 per-thread 的，不会被其他 CPU 写

// [X] 错误:English 散文解释
// The rcu_nesting field of struct thread is per-thread and not written by other CPUs

// [X] 错误:中文 token
// 线程结构体的 RCU嵌套计数 字段是每个线程各自的
```

### 规则 B:注释回答"为什么"，不复读"是什么"

```
// [X] 错误:复读代码
int i = 0;  // 设 i 为 0

// [OK] 正确:揭示隐含约束
// debug 用计数器，SMP 路径不保护它;TLB shootdown 的 IPI handler 也会写它
int i = 0;
```

### 规则 C:TODO / FIXME 三选一，禁止堆叠

```
// [OK] 正确
TODO(4GB+): 当前 pmm_total_pages 用 uint32_t，接入 >4GB 内存前需要扩

// [X] 错误:堆叠
// TODO FIXME

// [X] 错误:无内容
// XXX
```

- `TODO(条件): 描述` - 想做的增量，注明触发条件或负责人
- `FIXME: 描述` - 已知 bug/缺陷，当前绕过
- `XXX` - **禁用**.含义不明，无上下文

---

## 排版规则

### R1:控制流语句一律加 `{}`

`if`,`for`,`while` 即使 body 只有一行，也必须加 `{}`.不省略。

```c
// [OK] 正确
if (ptr == NULL) {
    return NULL;
}

// [X] 错误:单行未加 `{}`
if (ptr == NULL) return NULL;
```

### R2:关键字与左括号之间保留空格

`if`,`for`,`while`,`switch` 与 `(` 之间保留一个空格。

```c
// [OK] 正确
if (x > 0) {
    for (int i = 0; i < n; i++) {

// [X] 错误
if(x > 0) {
    for(int i = 0; i < n; i++) {
```

### R3:`{` 放在行尾(K&R 风格)

`if`,`for`,`while`，`else` 后的 `{` 放在当前行末尾，不另起一行。

```c
// [OK] 正确
if (cond) {
    stmt();
} else {
    other();
}

// [X] 错误
if (cond)
{
    stmt();
}
```

### R4:函数之间保留一个空行

相邻函数定义之间保留一个空行。函数体首尾 `{}` 紧贴代码，前后不保留多余空行。

```c
void foo(void)
{
    stmt();
}

void bar(void)
{
    stmt();
}
```

### R5:变量声明每行一个

每个变量声明独占一行，不出现 `int a， b;`.

```c
// [OK] 正确
int a;
int b;

// [X] 错误
int a, b;
```

### R6:`/**` 块内续行 `*` 后空一格

`/** ... */` 块内的续行与 `/* ... */` 块一致:`*` 后空一格再接文字。

```c
// [OK] 正确
/** Reader API
 *
 * rcu_read_lock / rcu_read_unlock 必须严格配对.
 */

// [X] 错误:多空格
/** Reader API
 *
 *  rcu_read_lock / rcu_read_unlock 必须严格配对.
```

有分层需要(如列表子项)时，在 `* ` 基准后追加缩进：

```c
/** synchronize_rcu - 阻塞调用方直到当前所有 reader 退出临界区
 *
 * 两阶段实现:
 *    阶段一:atomic_inc(gp_seq) 后自旋等待所有 CPU 观察到新 gp_seq
 *    阶段二:等待 blocked_tasks 链表清空
 */
```

### R7:类型转换 `(type*)` 不留空格

`*` 紧贴类型名，不在括号内加空格。

```c
// [X] 错误:空格在 * 前
uint64_t *ptr = (uint64_t *)addr;

// [OK] 正确:* 紧贴类型名
void *p = (void*)addr;
```

注意:`*` 后的空格用于修饰指针变量名(`uint64_t *ptr`)，不在类型转换括号内出现。

---

## 已发现的风格不一致

以下为当前代码中不符上述风格之处。对齐工作由仓主完成，本文档仅记录锚点。

| # | 文件 | 行 | 问题 | 建议操作 |
|---|------|----|------|---------|
| 1 | `kernel/lapic.h` (已删除的 x86-ism) | 1-15 | 顶注块用了 `/**`(档 2 风格)，按规则应为 `/*`(档 1 风格) | 改 `/**` -> `/*`，缩进对齐 |
| 2 | `kernel/thread.c` | 30 | `/* XXX */` 含义不明，且无上下文 | 删，或展开为 TODO |
| 3 | `kernel/thread.c` | 52 | `// TODO FIXME` 堆叠 | 选一个，补充描述 |
| 4 | `kernel/heap.h` | 6-7 | `#define KHEAP_VBASE` 上方 `// TODO FIXME` 不明确：哪里的 TODO | 补充条件或删 |
| 5 | `kernel/heap.h` | 8 | 孤立注释 `// 0xFFFFD00000000000`:是旧值还是注释掉的代码？ | 确认后删或加说明 |
| 6 | `kernel/heap.h` | 29 | 注释掉的 `// struct list_node head；` 死代码 | 删 |
| 7 | `kernel/thread.c` | 46-51 | `THREAD_STATUS_STR` 初始化含注释掉的旧字符串(`"READY "` 等)+ 带 `//` 的当前字符串混排 | 删掉注释掉的那些行 |

注意:`#7` 是特殊情况:`THREAD_STATUS_STR` 当前值(`" P  . "` 等)是用于 TTY 渲染的缩略显示(占 5 字符对齐).注释掉的行是旧版长字符串。删除注释掉的旧行后代码依然正确，因为当前值已在原地。不属于注释风格修复，是对齐过程顺便可做的清理。
