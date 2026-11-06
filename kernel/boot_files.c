#include "boot_files.h"
#include "cpio.h"
#include "string.h"

int boot_file_normalize(const char *path, char out[BOOT_FILE_PATH_MAX])
{
	if (!path || !out) {
		return -2;
	}
	size_t n = 0;
	while (*path) {
		while (*path == '/') {
			path++;
		}
		const char *start = path;
		while (*path && *path != '/') {
			if (*path == ':') {
				return -2;
			}
			path++;
		}
		size_t len = (size_t)(path - start);
		if (!len || (len == 1 && start[0] == '.')) {
			continue;
		}
		if (len == 2 && start[0] == '.' && start[1] == '.') {
			return -2;
		}
		if (len >= BOOT_FILE_PATH_MAX - n - 1) {
			return -2;
		}
		out[n++] = '/';
		memcpy(out + n, start, len);
		n += len;
	}
	out[n] = 0;
	return n ? 0 : -2;
}

static int valid_view(const void *data, uint64_t size)
{
	/* 下游 ELF 与配置解析接口以 int 表示长度，转换前检查其取值范围。 */
	return data && size <= 0x7fffffffULL && (uintptr_t)data <= UINTPTR_MAX - size;
}

int boot_files_from_modules(
	struct boot_file_set *set, const char *label, const struct boot_module_list *modules)
{
	if (!set || !label || !modules || (modules->count && !modules->items)) {
		return -2;
	}
	char path[BOOT_FILE_PATH_MAX];
	for (uint64_t i = 0; i < modules->count; i++) {
		const struct boot_module *m = &modules->items[i];
		if (!valid_view(m->address, m->size) || boot_file_normalize(m->path, path)) {
			return -2;
		}
	}
	*set = (struct boot_file_set){
		.label = label, .kind = BOOT_FILES_MODULES, .modules = modules};
	return 0;
}

int boot_files_from_cpio(
	struct boot_file_set *set, const char *label, const void *data, uint32_t size)
{
	struct cpio_reader reader;
	if (!set || !label || !valid_view(data, size) || cpio_reader_init(&reader, data, size)) {
		return -2;
	}
	struct cpio_entry entry;
	char path[BOOT_FILE_PATH_MAX];
	int ret;
	while ((ret = cpio_reader_next(&reader, &entry)) == 1) {
		if ((entry.mode & 0170000) == 0040000) {
			continue;
		}
		if ((entry.mode & 0170000) != 0100000 || boot_file_normalize(entry.name, path)) {
			return -2;
		}
	}
	if (ret != 0) {
		return -2;
	}
	*set = (struct boot_file_set){
		.label = label, .kind = BOOT_FILES_CPIO, .cpio = {data, size}};
	return 0;
}

int boot_file_lookup(const struct boot_file_set *set, const char *path, struct boot_file *file)
{
	char wanted[BOOT_FILE_PATH_MAX], normalized[BOOT_FILE_PATH_MAX];
	if (!set || !file || boot_file_normalize(path, wanted)) {
		return -2;
	}
	struct boot_file result = {.source = set};
	int found = 0;
	if (set->kind == BOOT_FILES_MODULES) {
		if (!set->modules) {
			return -2;
		}
		for (uint64_t i = 0; i < set->modules->count; i++) {
			const struct boot_module *m = &set->modules->items[i];
			if (boot_file_normalize(m->path, normalized)) {
				return -2;
			}
			if (strcmp(wanted, normalized)) {
				continue;
			}
			if (found++ || !valid_view(m->address, m->size)) {
				return -2;
			}
			result.data = m->address;
			result.size = m->size;
		}
	} else if (set->kind == BOOT_FILES_CPIO) {
		struct cpio_reader reader;
		struct cpio_entry entry;
		if (cpio_reader_init(&reader, set->cpio.data, set->cpio.size)) {
			return -2;
		}
		int ret;
		while ((ret = cpio_reader_next(&reader, &entry)) == 1) {
			if ((entry.mode & 0170000) == 0040000) {
				continue;
			}
			if ((entry.mode & 0170000) != 0100000) {
				return -2;
			}
			if (boot_file_normalize(entry.name, normalized)) {
				return -2;
			}
			if (strcmp(wanted, normalized)) {
				continue;
			}
			if (found++) {
				return -2;
			}
			result.data = entry.data;
			result.size = entry.size;
		}
		if (ret != 0) {
			return -2;
		}
	} else {
		return -2;
	}
	if (!found) {
		return -1;
	}
	memcpy(result.path, wanted, strlen(wanted) + 1);
	*file = result;
	return 0;
}

int boot_file_lookup_in(const struct boot_file_set *set, const char *directory, const char *name,
	struct boot_file *file)
{
	if (!name || !directory) {
		return -2;
	}
	for (const char *p = name; *p; p++) {
		if (*p == '/') {
			return boot_file_lookup(set, name, file);
		}
	}
	char path[BOOT_FILE_PATH_MAX];
	size_t a = strlen(directory), b = strlen(name);
	if (a + b + 2 > sizeof(path)) {
		return -2;
	}
	memcpy(path, directory, a);
	path[a] = '/';
	memcpy(path + a + 1, name, b + 1);
	return boot_file_lookup(set, path, file);
}
