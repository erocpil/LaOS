#include "task_conf.h"
#include "task_parser.h"
#include "heap.h"
#include "string.h"
#include "printf.h"

struct task_conf task_conf;
static struct boot_file_set default_files;

static void clear_records(struct task_conf *conf)
{
	struct list_node *pos, *next;
	list_for_each_safe(pos, next, &conf->head) {
		struct task *t = container_of(pos, struct task, node);
		list_del(pos);
		kfree(t);
	}
	list_for_each_safe(pos, next, &conf->directives) {
		struct selftest_directive *d = container_of(pos, struct selftest_directive, node);
		list_del(pos);
		kfree(d);
	}
}

int task_conf_init_ctx(
	struct task_conf *conf, const struct boot_file_set *files, enum task_conf_purpose purpose)
{
	if (!conf || !files || conf->initialized || (unsigned)purpose > TASK_CONF_COMPAT) {
		return -1;
	}
	memset(conf, 0, sizeof(*conf));
	list_init(&conf->head);
	list_init(&conf->directives);
	conf->files = files;
	conf->purpose = purpose;
	conf->initialized = true;
	return 0;
}

int task_conf_claim_cpu_ctx(struct task_conf *conf, uint32_t cpu_id)
{
	if (!conf || !conf->initialized || !conf->parsed || cpu_id >= 64 ||
		(conf->started_cpus & (1ULL << cpu_id))) {
		return -1;
	}
	conf->started_cpus |= 1ULL << cpu_id;
	return 0;
}

int task_conf_destroy_ctx(struct task_conf *conf)
{
	if (!conf || !conf->initialized || conf->started_cpus || conf->tests_started) {
		return -1;
	}
	clear_records(conf);
	memset(conf, 0, sizeof(*conf));
	return 0;
}

int task_conf_parse_ctx(struct task_conf *conf, const struct boot_file *file)
{
	if (!conf || !conf->initialized || conf->parsed || !file || file->source != conf->files ||
		!file->data || !file->size || file->size > 0x7fffffffU ||
		(uintptr_t)file->data > UINTPTR_MAX - file->size) {
		return -1;
	}
	int ret = task_conf_parse_v1_ctx(conf, file);
	if (ret) {
		kprintf("[boot-config] parse failed: %s:%s\n", conf->files->label, file->path);
		clear_records(conf);
		conf->module_missing_panic = false;
		return -1;
	}
	memcpy(conf->config_path, file->path, sizeof(conf->config_path));
	conf->parsed = true;
	return 0;
}

int task_conf_load_ctx(struct task_conf *conf, const char *path)
{
	struct boot_file file;
	if (!conf || !conf->initialized) {
		return -1;
	}
	int ret = boot_file_lookup(conf->files, path, &file);
	if (ret) {
		kprintf("[boot-config] lookup failed: %s:%s status=%d\n", conf->files->label, path,
			ret);
		return -1;
	}
	return task_conf_parse_ctx(conf, &file);
}

bool task_conf_has_ap_tasks_ctx(const struct task_conf *conf)
{
	if (!conf || !conf->initialized || !conf->parsed) {
		return false;
	}
	struct task *task;
	list_for_each_entry(task, &conf->head, node) {
		if (task->cpu_id > 0) {
			return true;
		}
	}
	return false;
}

int task_conf_add_directive_ctx(struct task_conf *conf, const struct selftest_directive *d)
{
	if (!conf || !conf->initialized || !d ||
		(conf->purpose != TASK_CONF_TEST && conf->purpose != TASK_CONF_COMPAT)) {
		return -1;
	}
	struct selftest_directive *copy = kmalloc(sizeof(*copy));
	if (!copy) {
		return -1;
	}
	memcpy(copy, d, sizeof(*copy));
	list_add_tail(&copy->node, &conf->directives);
	return 0;
}

bool task_conf_has_directives_ctx(const struct task_conf *conf)
{
	return conf && conf->initialized && conf->parsed && !list_empty(&conf->directives);
}

void task_conf_init(const struct boot_module_list *modules)
{
	if (!task_conf.initialized &&
		!boot_files_from_modules(&default_files, "compat-boot-files", modules)) {
		(void)task_conf_init_ctx(&task_conf, &default_files, TASK_CONF_COMPAT);
	}
}
int task_conf_parse_module(const struct boot_module *module)
{
	if (!module || !task_conf.initialized) {
		return -1;
	}
	struct boot_file file;
	if (boot_file_lookup(task_conf.files, module->path, &file)) {
		return -1;
	}
	return task_conf_parse_ctx(&task_conf, &file);
}
bool task_conf_has_ap_tasks(void)
{
	return task_conf_has_ap_tasks_ctx(&task_conf);
}
bool task_conf_has_directives(void)
{
	return task_conf_has_directives_ctx(&task_conf);
}
struct list_node *task_conf_get_directives(void)
{
	return &task_conf.directives;
}
