#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "task_conf.h"
#include "cpio.h"

#include "host_support.h"

static void *read_file(const char *name, uint32_t *size)
{
	FILE *f = fopen(name, "rb");
	assert(f);
	assert(!fseek(f, 0, SEEK_END));
	long n = ftell(f);
	assert(n > 0);
	rewind(f);
	void *p = malloc((size_t)n);
	assert(p);
	assert(fread(p, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	*size = (uint32_t)n;
	return p;
}
static int records(struct list_node *head)
{
	int n = 0;
	struct list_node *p;
	list_for_each(p, head) {
		n++;
	}
	return n;
}
static struct boot_file view(struct boot_file_set *set, const char *path)
{
	struct boot_file f;
	assert(boot_file_lookup(set, path, &f) == 0);
	return f;
}

int main(int argc, char **argv)
{
	assert(argc == 5);
	uint32_t n1, n2, n3;
	void *a1 = read_file(argv[1], &n1), *a2 = read_file(argv[2], &n2),
	     *a3 = read_file(argv[3], &n3);
	uint32_t n4;
	void *a4 = read_file(argv[4], &n4);
	struct boot_file_set dirs;
	assert(!boot_files_from_cpio(&dirs, "dirs", a4, n4));
	struct boot_file dirfile = view(&dirs, "/task/shared.mo");
	assert(!memcmp(dirfile.data, "DIRS", 4));
	assert(boot_file_lookup(&dirs, "/task", &dirfile) == -1);
	struct boot_file_set base, tests, duplicates;
	assert(!boot_files_from_cpio(&base, "base", a1, n1));
	assert(!boot_files_from_cpio(&tests, "tests", a2, n2));
	assert(!boot_files_from_cpio(&duplicates, "duplicate", a3, n3));
	assert(!cpio_init(a1, n1));
	struct cpio_entry entry;
	assert(cpio_next(&entry) == 1);
	const void *first = entry.data;
	struct boot_file f = view(&tests, "./task//shared.mo");
	assert(f.source == &tests && !memcmp(f.data, "TEST", 4));
	f = view(&base, "/task/shared.mo");
	assert(!memcmp(f.data, "BASE", 4));
	assert((uintptr_t)f.data % 4 == 0); /* Standard newc file-data alignment. */
	assert(boot_file_lookup(&base, "/task/shared", &f) == -1);
	assert(boot_file_lookup(&base, "/task/shared.mo.bak", &f) == -1);
	assert(boot_file_lookup(&base, "/task/only-test.mo", &f) == -1);
	assert(boot_file_lookup(&base, "task/../conf/task.conf", &f) == -2);
	assert(boot_file_lookup(&base, "tests:/task/shared.mo", &f) == -2);
	char longpath[BOOT_FILE_PATH_MAX + 8];
	memset(longpath, 'a', sizeof(longpath));
	longpath[sizeof(longpath) - 1] = 0;
	assert(boot_file_lookup(&base, longpath, &f) == -2);
	assert(boot_file_lookup(&duplicates, "/task/shared.mo", &f) == -2);
	/* Independent lookups must not reset the legacy archive's cursor. */
	assert(cpio_next(&entry) == 1 && entry.data != first && entry.size == 4);
	struct boot_file_set bad;
	for (uint32_t n = 0; n < n1; n++) {
		assert(boot_files_from_cpio(&bad, "truncated", a1, n) == -2);
	}
	unsigned char *corrupt = malloc(n1);
	assert(corrupt);
	memcpy(corrupt, a1, n1);
	corrupt[94] = 'Z';
	assert(boot_files_from_cpio(&bad, "bad-hex", corrupt, n1) == -2);
	memcpy(corrupt, a1, n1);
	memset(corrupt + 94, 'F', 8);
	assert(boot_files_from_cpio(&bad, "overflow", corrupt, n1) == -2);
	free(corrupt);

	struct task_conf b = {0}, t = {0};
	assert(!task_conf_init_ctx(&b, &base, TASK_CONF_BASE));
	assert(!task_conf_init_ctx(&t, &tests, TASK_CONF_TEST));
	assert(!task_conf_load_ctx(&b, "/conf/task.conf"));
	assert(!task_conf_load_ctx(&t, "/conf/test.conf"));
	assert(records(&b.head) == 1 && records(&t.head) == 1);
	assert(!task_conf_has_directives_ctx(&b) && task_conf_has_directives_ctx(&t));
	assert(b.module_missing_panic && !t.module_missing_panic);
	assert(!task_conf_has_ap_tasks_ctx(&b) && task_conf_has_ap_tasks_ctx(&t));
	struct selftest_directive *d =
		list_first_entry(&t.directives, struct selftest_directive, node);
	assert(!strcmp(d->name, "registry") && d->module_id == -1);
	assert(task_conf_load_ctx(&b, "/conf/task.conf") == -1);
	assert(task_conf_init_ctx(&b, &tests, TASK_CONF_TEST) == -1);
	assert(!task_conf_claim_cpu_ctx(&b, 0));
	assert(task_conf_claim_cpu_ctx(&b, 0) == -1 && task_conf_claim_cpu_ctx(&b, 64) == -1);
	assert(!task_conf_claim_cpu_ctx(&t, 0)); /* Same CPU, different contexts. */
	assert(task_conf_destroy_ctx(&b) == -1);
	/* No actual threads were launched by this host test. */
	b.started_cpus = t.started_cpus = 0;
	assert(!task_conf_destroy_ctx(&b) && !task_conf_destroy_ctx(&t));
	assert(!allocations);

	const char text[] = "@module_missing panic\n0 user.elf:init 3 0xc\n@test registry\n";
	struct boot_module item = {(void *)text, sizeof(text) - 1, "/conf/task.conf", NULL};
	struct boot_module_list list = {1, &item};
	struct boot_file_set external;
	assert(!boot_files_from_modules(&external, "external", &list));
	assert(!task_conf_init_ctx(&b, &external, TASK_CONF_BASE));
	assert(task_conf_load_ctx(&b, "/conf/task.conf") == -1); /* @test in base rejected. */
	assert(!allocations && list_empty(&b.head) && list_empty(&b.directives));
	assert(!b.module_missing_panic && !b.parsed);
	f = view(&tests, "/conf/test.conf");
	assert(task_conf_parse_ctx(&b, &f) == -1); /* Source mixing rejected. */
	assert(!task_conf_destroy_ctx(&b));
	for (int failure = 0; failure < 2; failure++) {
		assert(!task_conf_init_ctx(&t, &external, TASK_CONF_TEST));
		fail_after = failure;
		assert(task_conf_load_ctx(&t, "/conf/task.conf") == -1);
		fail_after = -1;
		assert(!allocations && list_empty(&t.head) && list_empty(&t.directives));
		assert(!task_conf_destroy_ctx(&t));
	}
	/* Syntax failure must roll back a previously allocated task and policy. */
	const char broken[] = "@module_missing panic\n0 user.elf:init 3 0xc\n@version 2\n";
	item.address = (void *)broken;
	item.size = sizeof(broken) - 1;
	assert(!task_conf_init_ctx(&b, &external, TASK_CONF_COMPAT));
	assert(task_conf_load_ctx(&b, "/conf/task.conf") == -1);
	assert(!allocations && !b.module_missing_panic && !b.parsed);
	assert(!task_conf_destroy_ctx(&b));
	/* Two normalized names in one external set are ambiguous, not first-wins. */
	struct boot_module items[] = {item, item};
	items[1].path = "./conf//task.conf";
	struct boot_module_list list2 = {2, items};
	assert(!boot_files_from_modules(&bad, "duplicate-modules", &list2));
	assert(boot_file_lookup(&bad, "/conf/task.conf", &f) == -2);
	assert(!boot_file_lookup(&external, "/conf/task.conf", &f));
	/* Unknown directives and overflowing integer fields are configuration errors. */
	const char *invalid[] = {"@unknown value\n", "999999999999999999999 user.elf:init 3 0xc\n",
		"x user.elf:init 3 0xc\n", "0 user.elf:init 3 0x\n",
		"0 user.elf:init 3 0xfffffffffffffffff\n"};
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		item.address = (void *)invalid[i];
		item.size = strlen(invalid[i]);
		assert(!task_conf_init_ctx(&b, &external, TASK_CONF_COMPAT));
		assert(task_conf_load_ctx(&b, "/conf/task.conf") == -1);
		assert(!allocations && !b.parsed);
		assert(!task_conf_destroy_ctx(&b));
	}
	free(a1);
	free(a2);
	free(a3);
	free(a4);
	puts("PASS: boot-file isolation, independent CPIO, configuration contexts, rollback and "
	     "ownership");
	return 0;
}
