/*
 * selftest.c - 内核自测试子系统实现
 *
 * 注册表：双向链表，按注册顺序排列。
 * 配置指令在 task_init() 之后、selftest_run() 之前应用。
 */

#include "selftest.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "export.h"
#include "module.h"

/* 前置声明以避免引入 cpu.h；模块 ABI 依赖的只读数据布局应保持稳定。 */
void cpu_enqueue_tail(int cpu_id, struct thread *t);

static struct list_node selftest_registry;
static struct list_node selftest_active;
static bool selftest_initialized;

static void selftest_ensure_initialized(void)
{
	if (selftest_initialized) {
		return;
	}

	list_init(&selftest_registry);
	list_init(&selftest_active);
	selftest_initialized = true;
}

/* 运行时实例：已注册 + 已配置（通过 directive apply） */
struct selftest_instance {
	const struct selftest *test;
	const struct module_desc *module; /* 模块测试对应的注册表描述符；内置测试为 NULL。 */
	bool configured;
	bool started;
	bool prepare_failed;
	struct list_node node;
};

int selftest_register(const struct selftest *test)
{
	if (!test || !test->name) {
		return -1;
	}

	selftest_ensure_initialized();

	struct selftest_instance *inst = kmalloc(sizeof(*inst));
	if (!inst) {
		return -1;
	}
	memset(inst, 0, sizeof(*inst));
	inst->test = test;

	list_add_tail(&inst->node, &selftest_registry);
	kprintf("[selftest] registered '%s'\n", test->name);

	return 0;
}
EXPORT_SYMBOL(selftest_register);

void selftest_apply_all(struct list_node *directives)
{
	if (!directives) {
		return;
	}

	selftest_ensure_initialized();

	struct selftest_directive *d;
	list_for_each_entry(d, directives, node) {
		/* 按名称查找已注册的测试。 */
		struct selftest_instance *inst;
		int found = 0;
		list_for_each_entry(inst, &selftest_registry, node) {
			if (strcmp(inst->test->name, d->name) != 0) {
				continue;
			}
			found = 1;

			/* 依次应用该指令中的键值配置。 */
			if (inst->test->configure) {
				for (int i = 0; i < d->kv_count; i++) {
					inst->test->configure(d->kvs[i].key, d->kvs[i].value);
				}
			}
			if (inst->test->prepare && inst->test->prepare() < 0) {
				inst->prepare_failed = true;
				kprintf("[selftest] WARNING: '%s' prepare failed\n", d->name);
			}
			inst->configured = true;

			/* 若测试由 .mo 载荷注册，关联其模块描述符。 */
			if (d->module_id >= 0) {
				inst->module =
					module_registry_find_by_id((unsigned int)d->module_id);
			}

			/* 将已配置测试移入活动列表。 */
			list_del(&inst->node);
			list_add_tail(&inst->node, &selftest_active);

			kprintf("[selftest] configured '%s' (%d kv pairs)\n", d->name, d->kv_count);
			break;
		}
		if (!found) {
			kprintf("[selftest] WARNING: '%s' not registered\n", d->name);
		}
	}
}

void selftest_run(void)
{
	selftest_ensure_initialized();

	/* 同一时刻只启动一个测试。当前测试完成后由 selftest_tick() 启动下一个，
	 * 避免多个测试同时使用 IPI 回调链或其他共享硬件资源。 */
	struct selftest_instance *inst;
	list_for_each_entry(inst, &selftest_active, node) {
		if (inst->started) {
			continue;
		}
		if (!inst->prepare_failed && inst->test->start) {
			kprintf("[selftest] starting '%s'\n", inst->test->name);
			inst->test->start();
		}
		inst->started = true;
		return; /* 本轮只启动一个测试实例。 */
	}
}

void selftest_tick(void)
{
	struct selftest_instance *inst = NULL;
	struct selftest_instance *tmp = NULL;
	struct list_node *pos, *n;

	/*
	 * Lazy-init 守卫：timer_handler 可能在 selftest_register() 之前
	 * 通过中断触发 selftest_tick()。直启路径无 constructor 注册且无
	 * selftest_load_payloads() 调用，链表保持 BSS 全零态，导致
	 * list_for_each_safe 解引用 NULL → Translation fault。
	 */
	selftest_ensure_initialized();

	list_for_each_safe(pos, n, &selftest_active) {
		inst = container_of(pos, struct selftest_instance, node);

		/* 后续测试由前一个测试完成后启动，此处跳过尚未开始的实例。 */
		if (!inst->started) {
			continue;
		}

		if (inst->prepare_failed || (inst->test->done && inst->test->done())) {
			bool ok = !inst->prepare_failed &&
				  (!inst->test->passed || inst->test->passed());
			kprintf("[selftest] '%s' %s\n", inst->test->name, ok ? "PASSED" : "FAILED");
			list_del(&inst->node);
			kfree(inst);

			/* 启动下一个尚未开始的测试实例。 */
			struct selftest_instance *next;
			list_for_each_entry(next, &selftest_active, node) {
				if (next->started) {
					continue;
				}
				if (!next->prepare_failed && next->test->start) {
					kprintf("[selftest] starting '%s'\n", next->test->name);
					next->test->start();
				}
				next->started = true;
				break;
			}
			continue;
		}
		if (inst->test->tick) {
			inst->test->tick();
		}
	}

	(void)tmp;
}

bool selftest_all_done(void)
{
	selftest_ensure_initialized();
	return list_empty(&selftest_active);
}

/*
 * selftest_load_payloads() - 遍历 directive 记录，同步加载 module= 指定的测试载荷。
 *
 * 对每个 selftest_directive，查找 module=<name> kv 对，从当前配置的文件集合
 * 中精确查找 /task/<name>，调用 selftest_load_payload() 同步加载。
 * 载荷的 selftest_init() 应在此阶段调用 selftest_register()。
 *
 * 此函数在 task_init() 之后、selftest_apply_all() 之前调用。
 */
#include "boot_info.h"

/* 由 elf_loader.c 提供。 */
extern int selftest_load_payload(uint8_t *elf_raw, int elf_size, const char *module_name);

void selftest_load_payloads_from_files(
	const struct boot_file_set *files, struct list_node *directives)
{
	if (!files || !directives) {
		return;
	}

	struct selftest_directive *d;
	list_for_each_entry(d, directives, node) {
		/* 查找 module= 配置项。 */
		const char *modname = NULL;
		for (int i = 0; i < d->kv_count; i++) {
			if (strcmp(d->kvs[i].key, "module") == 0) {
				modname = d->kvs[i].value;
				break;
			}
		}
		if (!modname) {
			continue;
		}

		struct boot_file file;
		int lookup = boot_file_lookup_in(files, "/task", modname, &file);
		if (lookup) {
			kprintf("[selftest] payload '%s' %s in %s\n", modname,
				lookup == -1 ? "not found" : "invalid/ambiguous", files->label);
			continue;
		}
		kprintf("[selftest] loading payload '%s' (%llu bytes) from %s:%s\n", modname,
			(unsigned long long)file.size, files->label, file.path);
		int mod_id = selftest_load_payload((uint8_t *)file.data, (int)file.size, modname);
		if (mod_id < 0) {
			kprintf("[selftest] payload '%s' load failed\n", modname);
		} else {
			d->module_id = mod_id;
		}
	}

	module_registry_dump();
}

/* 兼容旧调用方，将引导器模块列表包装为 boot_file_set 后复用统一加载路径。 */
void selftest_load_payloads(const struct boot_module_list *modules, struct list_node *directives)
{
	struct boot_file_set files;
	if (!boot_files_from_modules(&files, "compat-test-files", modules)) {
		selftest_load_payloads_from_files(&files, directives);
	}
}

/*
 * 模块使用的原子加法并返回新值接口。
 *
 * ARM64 模块不能依赖 libgcc 中的 __aarch64_ldadd8_rel；由内核导出此封装，
 * 使模块无需链接 libgcc。 */
uint64_t module_atomic_add_fetch(volatile uint64_t *ptr, uint64_t val)
{
	return __atomic_add_fetch(ptr, val, __ATOMIC_RELEASE);
}
EXPORT_SYMBOL(module_atomic_add_fetch);

struct thread *selftest_create_worker(int cpu, void (*entry)(void *), const char *name, void *data)
{
	struct thread *t = thread_create_on(entry, data, cpu);
	if (!t) {
		return NULL;
	}
	thread_set_name(t, name);

	return t;
}
EXPORT_SYMBOL(selftest_create_worker);

void selftest_start_worker(struct thread *t)
{
	if (t) {
		cpu_enqueue_tail(t->target_cpu, t);
	}
}
EXPORT_SYMBOL(selftest_start_worker);

void selftest_discard_worker(struct thread *t)
{
	if (t) {
		thread_destroy(t);
	}
}
EXPORT_SYMBOL(selftest_discard_worker);
