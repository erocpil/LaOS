#ifndef __ACPI_H__
#define __ACPI_H__

/*
 * ACPI RSDP、系统描述表头与 MCFG 数据结构。
 *
 * 定义 RSDP，SDT 头部，MCFG 条目等 ACPI 结构体。
 * 内核关心里面的 PCIe MMIO 基地址(MCFG).
 */

#include <stdint.h>

struct pmm_memmap;

/* 所有 ACPI 系统描述表共用的表头。 */
typedef struct {
	char Signature[4];
	uint32_t Length;
	uint8_t Revision;
	uint8_t Checksum;
	char OEMID[6];
	char OEMTableID[8];
	uint32_t OEMRevision;
	uint32_t CreatorID;
	uint32_t CreatorRevision;
} __attribute__((packed)) acpi_header_t;

/* 一个 PCI segment 的 ECAM 地址范围描述。 */
typedef struct {
	uint64_t BaseAddress;	  /* 该段 ECAM 区域的物理基地址。 */
	uint16_t PciSegmentGroup; /* PCI segment 组号。 */
	uint8_t StartBusNumber;	  /* 起始总线号。 */
	uint8_t EndBusNumber;	  /* 结束总线号。 */
	uint32_t Reserved;
} __attribute__((packed)) mcfg_entry_t;

/* MCFG 表头及可变长度的地址分配项。 */
typedef struct {
	acpi_header_t Header;
	uint64_t Reserved;
	mcfg_entry_t Entries[]; /* 可变长度的配置项列表。 */
} __attribute__((packed)) mcfg_table_t;

/* XSDT 表头及后续的 64 位物理地址表项。 */
typedef struct {
	acpi_header_t Header;
	uint64_t TablePointers[]; /* 指向其他 ACPI 表的物理地址。 */
} __attribute__((packed)) xsdt_t;

/* ACPI 2.0 及后续版本的 RSDP。 */
typedef struct {
	char Signature[8];
	uint8_t Checksum;
	char OEMID[6];
	uint8_t Revision;
	uint32_t RsdtAddress; // 32位 RSDT 地址
	uint32_t Length;
	uint64_t XsdtAddress; // 64位 XSDT 地址
	uint8_t ExtendedChecksum;
	uint8_t Reserved[3];
} __attribute__((packed)) rsdp_t;

uintptr_t find_mcfg_base(rsdp_t *rsdp, const struct pmm_memmap *memmap);

#endif
