/*
 * module.c - append-only 模块注册表
 *
 * 固定容量数组 (32 条目)，spinlock 保护，不提供 remove/unload。
 * 使用 reserve/commit/cancel 三态确保加载失败不产生半初始化记录。
 *
 * 设计约束：
 *  - reserve 必须发生在 selftest_init() 副作用之前，失败时安全取消
 *  - commit 之后条目对外可见，不可回滚
 *  - cancel 仅用于加载失败路径，释放 name 副本
 */

#include "module.h"
#include "heap.h"
#include "string.h"
#include "lock.h"
#include "printf.h"

#define MODULE_REGISTRY_CAPACITY 32

enum registry_state {
	REG_FREE = 0,
	REG_RESERVED,
	REG_COMMITTED,
};

static struct module_desc g_registry[MODULE_REGISTRY_CAPACITY];
static uint8_t            g_registry_state[MODULE_REGISTRY_CAPACITY];
static unsigned int       g_next_id;
static spinlock_t         g_registry_lock;
static bool               g_registry_inited;

static void registry_init(void)
{
	if (!g_registry_inited) {
		spin_lock_init(&g_registry_lock);
		g_registry_inited = true;
	}
}

int module_registry_reserve(struct module_desc *desc)
{
	registry_init();

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&g_registry_lock, flags);

	/* 找空闲槽位 */
	int slot = -1;
	for (int i = 0; i < MODULE_REGISTRY_CAPACITY; i++) {
		if (g_registry_state[i] == REG_FREE) {
			slot = i;
			break;
		}
	}

	if (slot < 0) {
		arch_spin_unlock_irqrestore(&g_registry_lock, flags);
		kprintf("[module] registry full (%u slots)\n",
				MODULE_REGISTRY_CAPACITY);
		return -1;
	}

	/* 拷贝 name 到持久存储 */
	char *name_copy = NULL;
	if (desc->name) {
		size_t len = strlen(desc->name);
		name_copy = kmalloc(len + 1);
		if (!name_copy) {
			arch_spin_unlock_irqrestore(&g_registry_lock, flags);
			return -1;
		}
		memcpy(name_copy, desc->name, len + 1);
	}

	/* 填充描述符，标记为 RESERVED（对外不可见） */
	unsigned int id = g_next_id++;
	g_registry[slot] = *desc;
	g_registry[slot].name = name_copy;
	g_registry[slot].id = id;
	g_registry_state[slot] = REG_RESERVED;

	arch_spin_unlock_irqrestore(&g_registry_lock, flags);

	return (int)id;
}

void module_registry_commit(unsigned int id)
{
	registry_init();

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&g_registry_lock, flags);

	for (int i = 0; i < MODULE_REGISTRY_CAPACITY; i++) {
		if (g_registry_state[i] == REG_RESERVED
				&& g_registry[i].id == id) {
			g_registry_state[i] = REG_COMMITTED;
			break;
		}
	}

	arch_spin_unlock_irqrestore(&g_registry_lock, flags);
}

void module_registry_cancel(unsigned int id)
{
	registry_init();

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&g_registry_lock, flags);

	for (int i = 0; i < MODULE_REGISTRY_CAPACITY; i++) {
		if (g_registry_state[i] == REG_RESERVED
				&& g_registry[i].id == id) {
			if (g_registry[i].name) {
				kfree((void*)g_registry[i].name);
			}
			memset(&g_registry[i], 0, sizeof(g_registry[i]));
			g_registry_state[i] = REG_FREE;
			break;
		}
	}

	arch_spin_unlock_irqrestore(&g_registry_lock, flags);
}

const struct module_desc *module_registry_find_by_id(unsigned int id)
{
	registry_init();

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&g_registry_lock, flags);

	for (int i = 0; i < MODULE_REGISTRY_CAPACITY; i++) {
		if (g_registry_state[i] == REG_COMMITTED
				&& g_registry[i].id == id) {
			const struct module_desc *desc = &g_registry[i];
			arch_spin_unlock_irqrestore(&g_registry_lock, flags);
			return desc;
		}
	}

	arch_spin_unlock_irqrestore(&g_registry_lock, flags);

	return NULL;
}

size_t module_registry_count(void)
{
	registry_init();

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&g_registry_lock, flags);

	size_t count = 0;
	for (int i = 0; i < MODULE_REGISTRY_CAPACITY; i++) {
		if (g_registry_state[i] == REG_COMMITTED) {
			count++;
		}
	}

	arch_spin_unlock_irqrestore(&g_registry_lock, flags);

	return count;
}

void module_registry_dump(void)
{
	registry_init();

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&g_registry_lock, flags);

	size_t used = 0;
	for (int i = 0; i < MODULE_REGISTRY_CAPACITY; i++) {
		if (g_registry_state[i] == REG_COMMITTED) {
			used++;
		}
	}

	kprintf("[module] registry: %zu/%u entries\n",
			used, MODULE_REGISTRY_CAPACITY);

	for (int i = 0; i < MODULE_REGISTRY_CAPACITY; i++) {
		if (g_registry_state[i] != REG_COMMITTED) {
			continue;
		}
		const struct module_desc *m = &g_registry[i];
		kprintf("  [%u] %-24s kind=%d base=%p size=%zu "
				"bss=%d entry=%p init=%p\n",
				m->id, m->name ? m->name : "(null)",
				m->kind, m->base, m->size,
				m->bss_count, m->entry, m->init);
	}

	arch_spin_unlock_irqrestore(&g_registry_lock, flags);
}
