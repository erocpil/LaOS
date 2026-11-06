/*
 * task.c — 任务、进程与线程生命周期管理
 *
 * 解析 task.conf 中的启动条目，并按任务类型创建用户态或内核态执行实体。
 */
#include <stddef.h>
#include "config.h"
#include "task.h"
#include "task_conf.h"
#include "thread.h"
#include "cpu.h"
#include "debug.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "vmm.h"
#include "hhdm.h"

#include "tests/test_mutex.h"

/* 每次启动单独复制参数；任务入口只借用，不得修改或释放 argv。 */
static int task_build_argv(struct task *t)
{
	enum { MAX_ARGS = 33 }; /* name + 16 positional + 16 key=value */
	size_t name_len = strlen(t->name) + 1;
	size_t size = (MAX_ARGS + 1) * sizeof(char *) + name_len + sizeof(t->args_buf) +
		      sizeof(t->kv_buf);
	char **argv = kmalloc(size);
	if (!argv) {
		return -1;
	}
	char *cursor = (char *)(argv + MAX_ARGS + 1);
	argv[0] = cursor;
	memcpy(cursor, t->name, name_len);
	cursor += name_len;
	memcpy(cursor, t->args_buf, sizeof(t->args_buf));
	char *p = cursor;
	cursor += sizeof(t->args_buf);
	int argc = 1;
	while (*p) {
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (!*p) {
			break;
		}
		if (argc == MAX_ARGS) {
			goto fail;
		}
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t') {
			p++;
		}
		if (*p) {
			*p++ = '\0';
		}
	}
	for (int i = 0; i < t->kv_count; i++) {
		if (argc == MAX_ARGS) {
			goto fail;
		}
		argv[argc++] = cursor;
		size_t key_len = strlen(t->kv_keys[i]);
		size_t value_len = strlen(t->kv_values[i]);
		memcpy(cursor, t->kv_keys[i], key_len);
		cursor += key_len;
		*cursor++ = '=';
		memcpy(cursor, t->kv_values[i], value_len + 1);
		cursor += value_len + 1;
	}
	argv[argc] = NULL;
	t->argc = argc;
	t->argv = argv;
	return 0;
fail:
	kfree(argv);
	return -1;
}

spinlock_t task_lock = SPINLOCK_INIT();

static const char *task_path[] = {
	"/task/",
	"/task/",
	"/task/",
	"/task/",
	"/data/",
	"/conf/",
};

/* 创建适配函数保留各任务类型的加载模式和参数契约。 */
typedef struct thread *(*task_create_fn)(const struct task *task, const struct boot_file *module);

static struct thread *task_create_driver(const struct task *task, const struct boot_file *module)
{
	struct thread *th = kthread_load_elf(
		(uint8_t *)module->data, module->size, KTHREAD_ELF_REL, task->module, task->data);
	if (th) {
		module_apply_kv_params(th, task->kv_count, task->kv_keys, task->kv_values);
	}
	return th;
}

static struct thread *task_create_thread(const struct task *task, const struct boot_file *module)
{
	return kthread_load_elf(
		(uint8_t *)module->data, module->size, KTHREAD_ELF_EXEC, task->module, task->data);
}

static struct thread *task_create_user(const struct task *task, const struct boot_file *module)
{
	/* 保留现有用户程序参数转换；加载器在返回前复制参数到用户栈。 */
	int id_val = 10;
	int code_prefix = 10;

	if (task->argc >= 2 && task->argv[1][0] != '0') {
		id_val = 0;
		for (const char *p = task->argv[1]; *p >= '0' && *p <= '9'; p++) {
			id_val = id_val * 10 + (*p - '0');
		}
	}
	if (task->argc >= 3 && task->argv[2][0] != '0') {
		code_prefix = 0;
		for (const char *p = task->argv[2]; *p >= '0' && *p <= '9'; p++) {
			code_prefix = code_prefix * 10 + (*p - '0');
		}
	}

	char var[64] = {'\0'};
	ksprintf(var, "id %d", id_val);
	char code[64] = {'\0'};
	ksprintf(code, "%d%d", code_prefix, id_val);
	char *argv[] = {"/bin/user_elf", var, code, NULL};
	return create_elf_process((uint8_t *)module->data, module->size, 3, argv);
}

/* 非可执行类型不提供创建函数；索引前须检查类型范围。 */
static const task_create_fn task_create_func[TASK_MAX] = {
	[TASK_DRIVER] = task_create_driver,
	[TASK_THREAD] = task_create_thread,
	[TASK_USER] = task_create_user,
};

static int find_module(
	const struct task_conf *conf, const char *module, int type, struct boot_file *file)
{
	if (type < 0 || type >= TASK_MAX || !conf->files) {
		return -2;
	}
	return boot_file_lookup_in(conf->files, task_path[type], module, file);
}

int task_load_conf(void)
{
	return task_conf_load_ctx(&task_conf, "/conf/task.conf");
}

#if CONFIG_TASK_PREEMPT_TEST
/* 抢占测试线程：
 *
 * 持锁期间请求重调度，随后释放锁并恢复抢占，以验证调度器的延迟抢占路径。
 * 仅在 CONFIG_TASK_PREEMPT_TEST 启用时构建。
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

int task_run_configured_cpu_ctx(struct task_conf *conf, const uint32_t cpu_id)
{
	kprintf("[task] scan cpu=%u\n", cpu_id);

	uint64_t flags = 0;
	arch_spin_lock_irqsave(&task_lock, flags);

	if (!conf || !conf->initialized || !conf->parsed || cpu_id >= MAX_CPUS ||
		task_conf_claim_cpu_ctx(conf, cpu_id)) {
		arch_spin_unlock_irqrestore(&task_lock, flags);
		return -1;
	}
	struct task *t = NULL;
	list_for_each_entry(t, &conf->head, node) {
		if (cpu_id == (const uint32_t)t->cpu_id) {
			if (t->type < 0 || t->type >= TASK_MAX || !task_create_func[t->type]) {
				kprintf("[task] unsupported executable task type: %d (%s)\n",
					t->type, t->module);
				continue;
			}
			L("searching %s @ %d type %d", t->module, t->cpu_id, t->type);
			struct boot_file file;
			int lookup = find_module(conf, t->module, t->type, &file);
			if (lookup) {
				if (lookup != -1) {
					panic("invalid or ambiguous component %s", t->module);
				}
				kprintf("[task] module not found: %s (type=%d)\n", t->module,
					t->type);
				if (conf->module_missing_panic) {
					panic("missing module %s", t->module);
				}
				continue;
			}
			kprintf("[task] %s resolved from %s:%s\n", t->module, conf->files->label,
				file.path);
			L("launching %s @ %d type %d", t->module, t->cpu_id, t->type);
			if (task_build_argv(t) < 0) {
				kprintf("[task] cannot build arguments for %s\n", t->name);
				continue;
			}
			struct thread *th = task_create_func[t->type](t, &file);
			if (th) {
				th->entry_args_owned = t->argv;
				th->entry_argc = t->argc;
				th->entry_argv = t->argv;
				if (th->is_user) {
					if (!th->pml4_phys || (uint64_t)th->pml4_phys ==
								      virt_to_phys(kernel_pml4)) {
						panic("user task must own a private page table");
					}
					kprintf("[task] private user root %s=%p\n", t->name,
						th->pml4_phys);
				}
				thread_set_name(th, t->name);
				thread_set_target_cpu(th, t->cpu_id);
				t->argv = NULL; /* 所有权已交给线程。 */
				t->argc = 0;
				cpu_enqueue_tail(t->cpu_id, th);
				kprintf("[task] queued %s from %s (type=%d)\n", th->name, t->module,
					t->type);
				L("launched task %s(%s) @ %d %d", th->name, t->module,
					th->target_cpu, t->cpu_id);
			} else {
				kfree(t->argv);
				t->argv = NULL;
				t->argc = 0;
				kprintf("[task] failed to launch %s from %s (type=%d)\n", t->name,
					t->module, t->type);
				L("Failed to launch task %s(%s) @ %d %d", t->name, t->module,
					cpu_id, t->cpu_id);
			}
		}
	}

	arch_spin_unlock_irqrestore(&task_lock, flags);
	return 0;
}

void task_run_configured_cpu(const uint32_t cpu_id)
{
	(void)task_run_configured_cpu_ctx(&task_conf, cpu_id);
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

int task_init_ctx(struct task_conf *conf, const struct boot_file_set *files,
	enum task_conf_purpose purpose, const char *config_path)
{
	if (task_conf_init_ctx(conf, files, purpose)) {
		return -1;
	}
	if (task_conf_load_ctx(conf, config_path)) {
		(void)task_conf_destroy_ctx(conf);
		return -1;
	}
	kprintf("[boot-config] purpose=%u selected %s:%s\n", (unsigned)purpose, files->label,
		conf->config_path);
	return 0;
}

int task_init(const struct boot_module_list *modules)
{
	static struct boot_file_set files;
	if (task_conf.initialized) {
		return -1;
	}
	if (boot_files_from_modules(&files, "boot-modules-compat", modules)) {
		return -1;
	}
	return task_init_ctx(&task_conf, &files, TASK_CONF_COMPAT, "/conf/task.conf");
}
