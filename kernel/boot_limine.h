#ifndef __BOOT_LIMINE_H__
#define __BOOT_LIMINE_H__

#include <limine.h>

#include "boot_info.h"

/* Convert Limine responses while bootloader-reclaimable memory is valid. */
int boot_info_from_limine(struct boot_info *info,
		const struct limine_hhdm_response *hhdm,
		const struct limine_memmap_response *memmap,
		const struct limine_module_response *modules,
		const struct limine_dtb_response *dtb);

#endif
