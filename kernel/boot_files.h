#ifndef LAOS_BOOT_FILES_H
#define LAOS_BOOT_FILES_H

#include <stddef.h>
#include <stdint.h>
#include "boot_info.h"

#define BOOT_FILE_PATH_MAX 256

/* 此视图不拥有 payload；使用期间必须保证其所属的启动信息仍然有效。 */
struct boot_file {
	const void *data;
	size_t size;
	char path[BOOT_FILE_PATH_MAX];
	const struct boot_file_set *source;
};

enum boot_file_kind { BOOT_FILES_MODULES, BOOT_FILES_CPIO };
struct boot_file_set {
	const char *label;
	enum boot_file_kind kind;
	union {
		const struct boot_module_list *modules;
		struct {
			const void *data;
			uint32_t size;
		} cpio;
	};
};

/* 成功返回 0，文件不存在时返回 -1，输入无效或匹配不唯一时返回 -2。
 * 查找不会回退到其他来源。 */
int boot_file_lookup(const struct boot_file_set *set, const char *path, struct boot_file *file);
int boot_files_from_modules(
	struct boot_file_set *set, const char *label, const struct boot_module_list *modules);
int boot_files_from_cpio(
	struct boot_file_set *set, const char *label, const void *data, uint32_t size);
int boot_file_lookup_in(const struct boot_file_set *set, const char *directory, const char *name,
	struct boot_file *file);
int boot_file_normalize(const char *path, char out[BOOT_FILE_PATH_MAX]);

#endif
