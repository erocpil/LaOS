/*
 * task.c - 进程/线程生命周期管理
 *
 * 消费 task.conf boot-time orchestration DSL，为每个 task entry 创建
 * 用户态或内核态线程。
 */
#include <stddef.h>
#include <stdbool.h>
#include "config.h"
#include "task.h"
#include "task_conf.h"
#include "thread.h"
#include "cpu.h"
#include "elf.h"
#include "ksym.h"
#include "debug.h"
#include "printf.h"
#include "string.h"
#include "log.h"

#include "mutex_test.h"

/* 将 args_buf 按空格切分为 argc/argv 数组。
 * argv[0] 设为模块名，argv[1..] 为 task.conf 第五列参数。
 * 字符串引用指向 args_buf 内用 '\0' 分隔的位置。 */
static void task_build_argv(struct task *t)
{
	char *p = t->args_buf;
	int idx = 0;

	t->argc = 0;
	if (t->args_buf[0] == '\0')
		return;

	/* argv[0] = 模块名 */
	t->argv[idx++] = t->name;
	t->argc++;

	/* 空格切分剩余参数 */
	while (*p && idx < 16) {
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0') break;
		t->argv[idx++] = p;
		t->argc++;
		while (*p && *p != ' ' && *p != '\t') p++;
		if (*p) { *p = '\0'; p++; }
	}
}

spinlock_t task_lock = SPINLOCK_INIT();

static const char *task_path[] = {
	"/task/", "/task/", "/task/", "/task/", "/data/", "/conf/",
};

/* 各任务类型的创建函数指针表.TASK_DRIVER / TASK_THREAD 走
 * task_run_cpu 内的 switch 直接调 kthread_load_elf 而未走此表；
 * 仅 TASK_USER 通过此表调 create_elf_process.保留表结构以便后续
 * TASK_DRIVER/THREAD 也收口至此。 */
static struct thread* (*task_create_func[])(uint8_t*, int, int, void*) = {
	[TASK_KERNEL] = NULL,
	[TASK_DRIVER] = NULL,
	[TASK_THREAD] = NULL,
	[TASK_USER]   = create_elf_process,
	[TASK_DATA]   = NULL,
	[TASK_CONFIG] = NULL,
};

static const struct boot_module *find_module(const char *module, int type)
{
	if (type < 0 || type >= TASK_MAX) {
		L("Invalid module type");
		return NULL;
	}

	const struct boot_module_list *modules = task_conf.modules;
	const struct boot_module *f = NULL;
	for (size_t i = 0; i < modules->count; i++) {
		const char *path = task_path[type];
		if (path && !strstr(modules->items[i].path, path)) {
			L("type %d %s continue", type, modules->items[i].path);
			continue;
		}
		if (strstr(modules->items[i].path, module)) {
			f = &modules->items[i];
			break;
		} else {
			L("skip %s", modules->items[i].path);
		}
	}

	if (f) {
		L("Found %s at %p size %lu bytes", module, f->address, f->size);
	} else {
		L("Not Found %s", module);
	}

	return f;
}

int task_load_conf()
{
	const struct boot_module *f = find_module("task.conf", TASK_CONFIG);

	if (!f) {
		L("Error: No module task.conf!");
		return -1;
	}

	return task_conf_parse_module(f);
}

#if CONFIG_TASK_PREEMPT_TEST
/** task_func_test_preempt - 抢占测试 kthread
 *
 * 持 spinlock，置 need_resched，解锁后 preempt_enable 兑现抢占，
 * 验证调度器抢占路径正常。仅 CONFIG_TASK_PREEMPT_TEST=1 时启动。
 */
void task_func_test_preempt(void *data)
{
	(void)data;
	spinlock_t lock = SPINLOCK_INIT();
	uint64_t flags = 0;
	struct thread *t = cpu_get_ctx()->current;
	while (1) {
		arch_spin_lock_irqsave(&lock, flags);
		L("%s %ld ticks %ld", t->name, t->id, t->ticks);
		cpu_get_ctx()->need_resched = 1;
		arch_spin_unlock_irqrestore(&lock, flags);
	}
}
#endif

void task_run_configured_cpu(const uint32_t cpu_id)
{
	kprintf("[task] scan cpu=%u\n", cpu_id);

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&task_lock, flags);

	struct task *t = NULL;
	list_for_each_entry(t, &task_conf.head, node) {
		if (cpu_id == (const uint32_t)t->cpu_id) {
			L("searching %s @ %d type %d", t->module, t->cpu_id, t->type);
			const struct boot_module *elf = find_module(t->module, t->type);
			if (!elf) {
				kprintf("[task] module not found: %s (type=%d)\n",
					t->module, t->type);
				if (task_conf.module_missing_panic)
					panic("missing module %s", t->module);
				continue;
			}
			struct thread *th = NULL;
			L("launching %s @ %d type %d", t->module, t->cpu_id, t->type);

			Elf64_Ehdr *ehdr = (Elf64_Ehdr*)elf->address;
			L("Elf64_Ehdr e_type %d", ehdr->e_type);
			/* Parse positional parameters before task-type-specific launch logic. */
			task_build_argv(t);
			switch (t->type) {
				case TASK_DRIVER:
				case TASK_THREAD:
					L("task type %d driver / thread", t->type);
					th = kthread_load_elf(elf->address, elf->size,
						(t->type == TASK_DRIVER) ? KTHREAD_ELF_REL : KTHREAD_ELF_EXEC,
						t->module, t->data);
					break;
				case TASK_USER:
					{
						/* 从 task.conf args_buf 解析参数：argv[1]=id, argv[2]=code_prefix.
						 * args_buf 为空时使用默认值 id=10, code_prefix=10。
						 * task_build_argv 已设置 t->argc/t->argv。 */
						int id_val = 10;
						int code_prefix = 10;

						if (t->argc >= 2 && t->argv[1][0] != '0') {
							id_val = 0;
							for (char *p = t->argv[1]; *p >= '0' && *p <= '9'; p++)
								id_val = id_val * 10 + (*p - '0');
						}
						if (t->argc >= 3 && t->argv[2][0] != '0') {
							code_prefix = 0;
							for (char *p = t->argv[2]; *p >= '0' && *p <= '9'; p++)
								code_prefix = code_prefix * 10 + (*p - '0');
						}

						char var[64] = { '\0' };
						ksprintf(var, "id %d", id_val);
						char code[64] = { '\0' };
						ksprintf(code, "%d%d", code_prefix, id_val);
						char *argv[] = { "/bin/user_elf", var, code, NULL };
						int argc = 3;
						th = task_create_func[t->type](elf->address, elf->size, argc, (void*)argv);
					}
					break;
				default:
					L("Error: unknown type %d", t->type);
					break;
			}
			if (th) {
				/* 应用 kv 参数（需要模块内存已加载）。 */
				module_apply_kv_params(th, t->kv_count,
						t->kv_keys, t->kv_values);
				th->entry_argc = t->argc;
				th->entry_argv = t->argv;
				thread_set_name(th, t->name);
				thread_set_target_cpu(th, t->cpu_id);
				cpu_enqueue_tail(t->cpu_id, th);
				kprintf("[task] queued %s from %s (type=%d)\n",
					th->name, t->module, t->type);
				L("launched task %s(%s) @ %d %d", th->name, t->module, th->target_cpu, t->cpu_id);
			} else {
				kprintf("[task] failed to launch %s from %s (type=%d)\n",
					t->name, t->module, t->type);
				L("Failed to launch task %s(%s) @ %d %d", t->name, t->module, cpu_id, t->cpu_id);
			}
		}
	}

	arch_spin_unlock_irqrestore(&task_lock, flags);
}

void task_run_cpu(const uint32_t cpu_id)
{
#if CONFIG_MUTEX_STRESS
	mutex_test_start_thread(cpu_id);
#endif

#if CONFIG_TASK_PREEMPT_TEST
	{
		struct thread *pt = thread_create(task_func_test_preempt, NULL);
		if (pt) {
			thread_set_name(pt, "tpreempt");
			cpu_enqueue_tail(cpu_id, pt);
		}
	}
#endif

	task_run_configured_cpu(cpu_id);
}

void task_run(void)
{
	task_run_cpu(cpu_get_ctx()->id);
}

int task_init(const struct boot_module_list *modules)
{
	L_TAG(LOG_MODULE, "Loading modules ...\n");
	ksym_dump_all();

	// 检查请求是否成功
	if (modules == NULL) {
		L("Error: No modules found!");
		panic("CPU %d", cpu_get_ctx()->id);
	} else if (modules->count == 0) {
		L("module count 0");
		return -1;
	}
	L_TAG(LOG_MODULE, "Found %d modules.\n", modules->count);

	task_conf_init(modules);

	if (task_load_conf() != 0)
		return -1;

	L("task conf:");
	struct task *t = NULL;
	list_for_each_entry(t, &task_conf.head, node) {
		L("cpu %d module %s name %s type %d magic %p",
				t->cpu_id, t->module, t->name, t->type, (void*)t->magic);
	}

	return 0;
}
