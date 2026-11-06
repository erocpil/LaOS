/* Parse a real archive through the kernel's file/config interfaces, no boot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot_files.h"
#include "task_conf.h"
#include "cpio.h"
#include "elf.h"
#include "host_support.h"

static int elf_matches(const struct boot_file *file, unsigned machine, unsigned type)
{
	Elf64_Ehdr hdr;
	if (file->size < sizeof(hdr)) {
		return -1;
	}
	memcpy(&hdr, file->data, sizeof(hdr));
	return memcmp(hdr.e_ident, "\177ELF", 4) || hdr.e_ident[EI_CLASS] != ELFCLASS64 ||
			       hdr.e_ident[EI_DATA] != ELFDATA2LSB ||
			       hdr.e_ident[EI_VERSION] != EV_CURRENT ||
			       hdr.e_ehsize != sizeof(hdr) || hdr.e_machine != machine ||
			       hdr.e_type != type
		       ? -1
		       : 0;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s archive machine-id\n", argv[0]);
		return 2;
	}
	int result = 1;
	FILE *stream = fopen(argv[1], "rb");
	if (!stream) {
		perror(argv[1]);
		return 1;
	}
	if (fseek(stream, 0, SEEK_END)) {
		fclose(stream);
		return 1;
	}
	long length = ftell(stream);
	if (length <= 0 || length > 0x7fffffffL) {
		fclose(stream);
		return 1;
	}
	rewind(stream);
	void *bytes = malloc((size_t)length);
	if (!bytes) {
		fclose(stream);
		return 1;
	}
	if (fread(bytes, 1, (size_t)length, stream) != (size_t)length) {
		fclose(stream);
		free(bytes);
		return 1;
	}
	fclose(stream);
	unsigned machine = (unsigned)strtoul(argv[2], NULL, 10);
	struct boot_file_set files;
	struct task_conf conf = {0};
	if (boot_files_from_cpio(&files, "base-initrd", bytes, (uint32_t)length)) {
		fprintf(stderr, "invalid CPIO file collection\n");
		goto out;
	}
	if (task_conf_init_ctx(&conf, &files, TASK_CONF_BASE) ||
		task_conf_load_ctx(&conf, "/conf/task.conf")) {
		fprintf(stderr, "invalid/missing base conf/task.conf\n");
		goto out;
	}
	struct cpio_reader reader;
	struct cpio_entry entry;
	unsigned count = 0, tasks = 0;
	if (cpio_reader_init(&reader, bytes, (uint32_t)length)) {
		goto out;
	}
	while (cpio_reader_next(&reader, &entry) == 1) {
		if ((entry.mode & 0170000) == 0040000) {
			continue;
		}
		struct boot_file file;
		if (boot_file_lookup(&files, entry.name, &file)) {
			fprintf(stderr, "ambiguous file: %s\n", entry.name);
			goto out;
		}
		count++;
	}
	struct task *task;
	list_for_each_entry(task, &conf.head, node) {
		struct boot_file file;
		if (boot_file_lookup_in(&files, "/task", task->module, &file)) {
			fprintf(stderr, "missing component: %s\n", task->module);
			goto out;
		}
		if (task->type < TASK_DRIVER || task->type > TASK_CONFIG || task->cpu_id < 0 ||
			task->cpu_id >= 64) {
			fprintf(stderr, "invalid base task: %s\n", task->module);
			goto out;
		}
		if (task->type <= TASK_USER &&
			elf_matches(&file, machine, task->type == TASK_DRIVER ? ET_REL : ET_EXEC)) {
			fprintf(stderr, "wrong ELF architecture/type: %s\n", task->module);
			goto out;
		}
		tasks++;
	}
	if (!tasks) {
		fprintf(stderr, "base config has no tasks\n");
		goto out;
	}
	printf("PASS: kernel parser accepted base initrd: files=%u tasks=%u\n", count, tasks);
	result = 0;
out:
	if (conf.initialized && task_conf_destroy_ctx(&conf)) {
		result = 1;
	}
	if (allocations) {
		fprintf(stderr, "configuration allocations leaked\n");
		result = 1;
	}
	free(bytes);
	return result;
}
