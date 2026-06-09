#include "boot_limine.h"

#include "string.h"

#define BOOT_LIMINE_MAX_MEMMAP_ENTRIES 128
#define BOOT_LIMINE_MAX_MODULES 64

static struct pmm_memmap_entry s_memmap[BOOT_LIMINE_MAX_MEMMAP_ENTRIES];
static struct boot_module s_modules[BOOT_LIMINE_MAX_MODULES];

int boot_info_from_limine(struct boot_info *info,
		const struct limine_hhdm_response *hhdm,
		const struct limine_memmap_response *memmap,
		const struct limine_module_response *modules,
		const struct limine_dtb_response *dtb)
{
	if (!info || !hhdm || !memmap ||
			memmap->entry_count > BOOT_LIMINE_MAX_MEMMAP_ENTRIES) {
		return -1;
	}
	if (modules && modules->module_count > BOOT_LIMINE_MAX_MODULES) {
		return -1;
	}

	memset(info, 0, sizeof(*info));
	info->hhdm_offset = hhdm->offset;
	info->memory_map.count = memmap->entry_count;
	info->memory_map.entries = s_memmap;

	for (uint64_t i = 0; i < memmap->entry_count; i++) {
		const struct limine_memmap_entry *src = memmap->entries[i];
		s_memmap[i].base = src->base;
		s_memmap[i].length = src->length;
		s_memmap[i].type = (int)src->type;
	}

	if (modules) {
		info->modules.count = modules->module_count;
		info->modules.items = s_modules;
		for (uint64_t i = 0; i < modules->module_count; i++) {
			const struct limine_file *src = modules->modules[i];
			s_modules[i].address = src->address;
			s_modules[i].size = src->size;
			s_modules[i].path = src->path;
			s_modules[i].string = src->string;
		}
	}

	if (dtb) {
		info->dtb = dtb->dtb_ptr;
	}

	return 0;
}
