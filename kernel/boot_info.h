#ifndef __BOOT_INFO_H__
#define __BOOT_INFO_H__

#include <stdint.h>

#include "pmm.h"

/* Bootloader-independent input consumed by the common kernel. */
struct boot_module {
	void *address;
	uint64_t size;
	const char *path;
	const char *string;
};

struct boot_module_list {
	uint64_t count;
	struct boot_module *items;
};

struct boot_info {
	uint64_t hhdm_offset;
	struct pmm_memmap memory_map;
	struct boot_module_list modules;
	const void *dtb;
};

#endif
