/*
 * x86_64 启动阶段的 ACPI 根表与 MCFG 查找。
 *
 * 只访问位于 Limine 内存映射中的表，并在读取表体前校验长度、边界和校验和。
 */

#include <stddef.h>

#include "acpi.h"
#include "pmm.h"
#include "string.h"
#include "hhdm.h"
#include "debug.h"

#define ACPI_RSDP_V1_SIZE   20
#define ACPI_MAX_RSDP_SIZE  4096
#define ACPI_MAX_TABLE_SIZE (1024 * 1024)

static int physical_range_valid(const struct pmm_memmap *memmap, uint64_t base, uint64_t size)
{
	if (!memmap || !memmap->entries || !base || !size || base > UINT64_MAX - size) {
		return 0;
	}

	for (uint64_t i = 0; i < memmap->count; i++) {
		const struct pmm_memmap_entry *entry = &memmap->entries[i];
		if (!entry->length || entry->base > UINT64_MAX - entry->length) {
			continue;
		}
		if (base >= entry->base && base + size <= entry->base + entry->length &&
			entry->type != PMM_MEMMAP_BAD_MEMORY &&
			entry->type != PMM_MEMMAP_FRAMEBUFFER) {
			return 1;
		}
	}

	return 0;
}

static int checksum_valid(const void *data, uint32_t length)
{
	const uint8_t *bytes = data;
	uint8_t sum = 0;

	for (uint32_t i = 0; i < length; i++) {
		sum = (uint8_t)(sum + bytes[i]);
	}

	return sum == 0;
}

static acpi_header_t *map_sdt(uint64_t phys, const struct pmm_memmap *memmap)
{
	uint64_t offset = hhdm_offset();
	if (phys > UINTPTR_MAX - offset ||
		!physical_range_valid(memmap, phys, sizeof(acpi_header_t))) {
		return NULL;
	}

	acpi_header_t *header = phys_to_virt(phys);
	uint32_t length = header->Length;
	if (length < sizeof(*header) || length > ACPI_MAX_TABLE_SIZE ||
		!physical_range_valid(memmap, phys, length)) {
		return NULL;
	}
	return header;
}

static int rsdp_valid(rsdp_t *rsdp, const struct pmm_memmap *memmap)
{
	if ((uintptr_t)rsdp < hhdm_offset()) {
		return 0;
	}
	uint64_t phys = virt_to_phys(rsdp);
	if (!physical_range_valid(memmap, phys, ACPI_RSDP_V1_SIZE) ||
		memcmp(rsdp->Signature, "RSD PTR ", 8) != 0 ||
		!checksum_valid(rsdp, ACPI_RSDP_V1_SIZE)) {
		return 0;
	}

	if (rsdp->Revision < 2) {
		return 1;
	}

	if (!physical_range_valid(memmap, phys, sizeof(rsdp_t))) {
		return 0;
	}
	uint32_t length = rsdp->Length;
	if (length < sizeof(rsdp_t) || length > ACPI_MAX_RSDP_SIZE ||
		!physical_range_valid(memmap, phys, length)) {
		return 0;
	}
	return checksum_valid(rsdp, length);
}

static uintptr_t scan_root_table(uint64_t root_phys, uint32_t entry_size, const char *signature,
	const struct pmm_memmap *memmap)
{
	acpi_header_t *root = map_sdt(root_phys, memmap);
	if (!root || memcmp(root->Signature, signature, 4) != 0) {
		L("[ACPI] Root table is outside the mapped memory range or has an invalid length.");
		return 0;
	}

	uint32_t payload_size = root->Length - sizeof(*root);
	if (payload_size % entry_size || !checksum_valid(root, root->Length)) {
		L("[ACPI] Root table has invalid entries or checksum.");
		return 0;
	}

	const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
	uint32_t entry_count = payload_size / entry_size;
	for (uint32_t i = 0; i < entry_count; i++) {
		uint64_t table_phys = 0;
		if (entry_size == sizeof(uint64_t)) {
			memcpy(&table_phys, entries + i * entry_size, sizeof(uint64_t));
		} else {
			uint32_t address32;
			memcpy(&address32, entries + i * entry_size, sizeof(address32));
			table_phys = address32;
		}

		acpi_header_t *table = map_sdt(table_phys, memmap);
		if (!table || !checksum_valid(table, table->Length)) {
			L("[ACPI] Skipping inaccessible or invalid table at 0x%lx.", table_phys);
			continue;
		}
		if (memcmp(table->Signature, "MCFG", 4) != 0) {
			continue;
		}

		if (table->Length < sizeof(mcfg_table_t)) {
			L("[ACPI] MCFG table is shorter than its fixed header.");
			continue;
		}
		uint32_t mcfg_payload = table->Length - sizeof(mcfg_table_t);
		if (mcfg_payload % sizeof(mcfg_entry_t)) {
			L("[ACPI] MCFG table has a partial address allocation entry.");
			continue;
		}

		mcfg_table_t *mcfg = (mcfg_table_t *)table;
		uint32_t count = mcfg_payload / sizeof(mcfg_entry_t);
		for (uint32_t j = 0; j < count; j++) {
			mcfg_entry_t entry;
			memcpy(&entry, &mcfg->Entries[j], sizeof(entry));
			if (entry.BaseAddress && entry.StartBusNumber <= entry.EndBusNumber) {
				return (uintptr_t)entry.BaseAddress;
			}
		}
	}

	L("[ACPI] MCFG table with a valid allocation was not found.");
	return 0;
}

/* 优先查找 XSDT；未找到有效 MCFG 时回退到 RSDT。 */
uintptr_t find_mcfg_base(rsdp_t *rsdp, const struct pmm_memmap *memmap)
{
	if (!rsdp || !rsdp_valid(rsdp, memmap)) {
		L("[ACPI] Invalid or inaccessible RSDP.");
		return 0;
	}

	if (rsdp->Revision >= 2 && rsdp->XsdtAddress) {
		uintptr_t mcfg =
			scan_root_table(rsdp->XsdtAddress, sizeof(uint64_t), "XSDT", memmap);
		if (mcfg) {
			return mcfg;
		}
	}

	return scan_root_table(rsdp->RsdtAddress, sizeof(uint32_t), "RSDT", memmap);
}
