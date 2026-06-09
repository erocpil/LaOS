#ifndef __LIST_H__
#define __LIST_H__

/*
 * list.h - 侵入式双向链表与 RCU 变体
 */

#include <stddef.h>

/** 侵入式链表节点 */
struct list_node {
	struct list_node *next, *prev;
};

/** 静态初始化宏 */
#define LIST_NODE_INIT(name) { &(name), &(name) }

#define LIST_NODE(name) \
	struct list_node name = LIST_NODE_INIT(name)

#define INIT_LIST_NODE(node) \
	do { \
		__typeof__(node) _n = (node); \
		_n->prev = _n; \
		_n->next = _n; \
	} while (0)

/** 运行时初始化函数 */
static inline void list_init(struct list_node *ptr)
{
	ptr->next = ptr;
	ptr->prev = ptr;
}

/** 在两个已知节点之间插入新节点 */
static inline void __list_add(struct list_node *newn,
		struct list_node *prev, struct list_node *next)
{
	next->prev = newn;
	newn->next = next;
	newn->prev = prev;
	prev->next = newn;
}

/** 将节点插入到链表头部(常用于做栈) */
static inline void list_add(struct list_node *newn, struct list_node *head)
{
	__list_add(newn, head, head->next);
}

/** 将节点插入到链表尾部(常用于做 FIFO 队列) */
static inline void list_add_tail(struct list_node *newn, struct list_node *head)
{
	__list_add(newn, head->prev, head);
}

/** 删除两个节点之间的元素 */
static inline void __list_del(struct list_node *prev, struct list_node *next)
{
	next->prev = prev;
	prev->next = next;
}

/** 从链表中删除一个节点 */
static inline void list_del(struct list_node *entry)
{
	__list_del(entry->prev, entry->next);
	entry->next = NULL; // 置空防止二次删除
	entry->prev = NULL;
}

static inline void list_del_init(struct list_node *entry) {
	__list_del(entry->prev, entry->next);
	INIT_LIST_NODE(entry); // 重新初始化节点
}

/** 检查链表是否为空 */
static inline int list_empty(const struct list_node *head)
{
	return head->next == head;
}

/** 从 list_node 成员找回宿主结构体首地址 */
#ifndef container_of
#define container_of(ptr, type, member) ({                      \
		const __typeof__( ((type *)0)->member ) *__mptr = (ptr);    \
		(type *)( (char*)__mptr - offsetof(type,member) );})
#endif

/** 获取链表第一个元素的宿主结构体指针
 * 调用者需确保链表非空
 */
#define list_first_entry(ptr, type, member) \
	container_of((ptr)->next, type, member)

#define list_last_entry(ptr, type, member) \
	container_of((ptr)->prev, type, member)

/** 遍历宏：简单遍历 */
#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

/** 安全遍历宏：允许在循环中执行 list_del
 * 必须用变量 'n' 暂存下一个节点的地址
 */
#define list_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; pos != (head); \
			pos = n, n = pos->next)

/** 宿主结构体遍历宏 */
#define list_for_each_entry(pos, head, member)                          \
	for (pos = container_of((head)->next, __typeof__(*pos), member);      \
			&pos->member != (head);                                        \
			pos = container_of(pos->member.next, __typeof__(*pos), member))

#define list_for_each_entry_reverse(pos, head, member) \
	for (pos = list_last_entry(head, __typeof__(*pos), member); \
			&pos->member != (head); \
			pos = container_of(pos->member.prev, __typeof__(*pos), member))

/** list_size - 计算链表中的元素个数
 *
 * 该操作的时间复杂度为 O(N)。链表为空时返回 0。
 */
static inline size_t list_size(const struct list_node *head)
{
	size_t count = 0;
	struct list_node *pos;

	list_for_each(pos, head) {
		count++;
	}

	return count;
}

/*
 *  RCU 变体 API
 *
 *  适用场景：读多写少的链表，希望 reader 走完全无锁路径。
 *  典型例子：pci 设备列表(init 写一次，运行时全是 reader)，
 *            task / thread 列表(fork/exit 少写，调度/ps 多读)，
 *            stats(统计多读，累加少写)。
 *
 *  调用约束(writer):
 *  - writer 之间仍需互斥(spin_lock 或其他序列化机制)
 *  - list_del_rcu 摘除后，节点的 next/prev 不可立即清零：
 *    并发 reader 可能仍在遍历，需要 next/prev 维持原值直到
 *    grace period 结束。释放节点必须在 synchronize_rcu() 之后。
 *  - list.h RCU 变体本身不调 rcu_*，由调用方在 writer 流程中
 *    自己负责 synchronize_rcu()
 *
 *  调用约束(reader):
 *  - 遍历必须包裹在 rcu_read_lock / rcu_read_unlock 之内
 *  - 遍历过程中可以 dereference 节点字段，但**不可保存节点
 *    指针出临界区**：出临界区后 grace period 可能完成，
 *    节点随时可能被 writer 释放
 *
 *  内存序：
 *  - writer 用 store-release 发布 prev->next，保证节点字段和 next
 *    链接先于节点对 reader 可见
 *  - reader 用 load-acquire 读取正向链接；一旦观察到新节点，随后
 *    读取该节点字段时也能观察到 writer 发布前的初始化
 *  - 编译器原子内建在 x86_64 上通常仍是普通 mov，在 ARM64 上
 *    生成 LDAR/STLR（或等价序列），因此公共 API 不依赖 x86 TSO
 *  - 该保证只覆盖正向 next 遍历。prev 供 writer 维护链表结构，
 *    不构成 RCU 反向遍历 API
 *
 *  未实现 call_rcu():
 *  - writer 删除节点后需要释放，目前唯一手段是同步 synchronize_rcu()
 *    后 kfree.grace period 量级 100ms~秒，writer 慢路径专用
 *  - 详 docs/rcu-design.md - 待 call_rcu 实现后此处补
 *    list_*_rcu 文档:writer 删除节点改写法为
 *        list_del_rcu(&n->node);
 *        call_rcu(&n->rcu_head, free_cb);
 *
 *  锁与中断：
 *  - list.h RCU 变体不触发任何锁，不关中断，不调 schedule()
 *  - reader 可在抢占临界区内调用；不可在 IRQ 顶半部调用
 *    (rcu_read_lock 自身约束，与 list 无关)
 */

/** RCU 链接的 acquire-load / release-store。
 *
 * 作用域限 list.h 内部，避免把只适用于链接指针的操作误当成
 * 通用原子 API。参数必须是可取地址、由单 writer 更新的链接字段。
 */
#define __list_LOAD_ACQUIRE(x) \
	__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define __list_STORE_RELEASE(x, val) \
	__atomic_store_n(&(x), (val), __ATOMIC_RELEASE)

/** 内部函数（RCU 变体）：在两个已知节点之间插入新节点
 *
 * 关键差异：先把 newn 自身的 next/prev 写好，再用 release-store
 * 发布到 prev->next.reader 通过 acquire-load 读取 prev->next，
 * 要么看到旧值(newn 不可见)，要么看到 newn：且看到时 newn
 * 自身的 next/prev 已经初始化完成，遍历可继续。
 *
 * next->prev 的更新不影响 reader 正向遍历(reader 只用 next)，
 * 反向遍历用例较少且当前测试未覆盖，参考实现仍写 prev。
 */
static inline void __list_add_rcu(struct list_node *newn,
		struct list_node *prev, struct list_node *next)
{
	newn->next = next;
	newn->prev = prev;
	next->prev = newn;
	__list_STORE_RELEASE(prev->next, newn);
}

/** 将节点插入到链表头部(RCU 变体) */
static inline void list_add_rcu(struct list_node *newn, struct list_node *head)
{
	__list_add_rcu(newn, head, head->next);
}

/** 将节点插入到链表尾部(RCU 变体) */
static inline void list_add_tail_rcu(struct list_node *newn, struct list_node *head)
{
	__list_add_rcu(newn, head->prev, head);
}

/** 从链表中删除一个节点(RCU 变体)
 *
 * 关键差异：
 * - 不清空 entry->next / entry->prev:并发 reader 可能正
 *   停留在 entry 上，下一步要 entry->next 继续遍历
 * - 用 release-store 把 entry 从 prev->next 摘除
 * - 调用方必须在 synchronize_rcu() 之后才能 kfree(entry)
 */
static inline void list_del_rcu(struct list_node *entry)
{
	__list_STORE_RELEASE(entry->prev->next, entry->next);
	entry->next->prev = entry->prev;
	/* 不清 entry->next / entry->prev，等 grace period 后释放 */
}

/** RCU 遍历宏(链表节点版)
 *
 * 调用约束：必须在 rcu_read_lock / rcu_read_unlock 之间使用
 */
#define list_for_each_rcu(pos, head) \
	for (pos = __list_LOAD_ACQUIRE((head)->next); \
			pos != (head); \
			pos = __list_LOAD_ACQUIRE(pos->next))

/** RCU 遍历宏(宿主结构体版，最常用)
 *
 * 调用约束：必须在 rcu_read_lock / rcu_read_unlock 之间使用
 *
 * 使用示例：
 *      rcu_read_lock();
 *      list_for_each_entry_rcu(dev, &pci_devices, node) {
 *          // 安全 dereference dev->any_field
 *      }
 *      rcu_read_unlock();
 */
#define list_for_each_entry_rcu(pos, head, member)                            \
	for (pos = container_of(__list_LOAD_ACQUIRE((head)->next),               \
				__typeof__(*pos), member);                                    \
				&pos->member != (head);                                              \
				pos = container_of(__list_LOAD_ACQUIRE(pos->member.next),           \
					__typeof__(*pos), member))

/** 取首元素(RCU 变体)
 *
 * 注意：仍要求链表非空.RCU 场景下"非空"判断本身竞争，
 * 常用模式是 list_first_or_null_rcu(返回 NULL 表示空).
 * 暂未需要该变体，待第一个 RCU 迁移点出现时再补。
 */
#define list_first_entry_rcu(ptr, type, member) \
	container_of(__list_LOAD_ACQUIRE((ptr)->next), type, member)

#endif
