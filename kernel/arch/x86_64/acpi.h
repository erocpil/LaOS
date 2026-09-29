#ifndef __ACPI_H__
#define __ACPI_H__

/*
 * acpi.h - ACPI 标准表头与 MCFG 结构定义
 *
 * 定义 RSDP，SDT 头部，MCFG 条目等 ACPI 结构体。
 * 内核关心里面的 PCIe MMIO 基地址(MCFG).
 */

#include <stdint.h>

// ACPI 标准表头 (所有表通用)
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

// MCFG 配置项结构
typedef struct {
	uint64_t BaseAddress;      // 该段 PCIe 配置空间的物理基地址
	uint16_t PciSegmentGroup;  // PCI 段组号
	uint8_t StartBusNumber;    // 起始总线号
	uint8_t EndBusNumber;      // 结束总线号
	uint32_t Reserved;
} __attribute__((packed)) mcfg_entry_t;

// MCFG 表完整结构
typedef struct {
	acpi_header_t Header;
	uint64_t Reserved;
	mcfg_entry_t Entries[];    // 可变长度的配置项列表
} __attribute__((packed)) mcfg_table_t;

// XSDT 表结构 (包含指向其他表的指针)
typedef struct {
	acpi_header_t Header;
	uint64_t TablePointers[];  // 指向其他 ACPI 表的 64 位物理地址
} __attribute__((packed)) xsdt_t;

// 假设这是从引导加载程序传来的 RSDP 指针
typedef struct {
	char Signature[8];
	uint8_t Checksum;
	char OEMID[6];
	uint8_t Revision;
	uint32_t RsdtAddress;      // 32位 RSDT 地址
	uint32_t Length;
	uint64_t XsdtAddress;      // 64位 XSDT 地址
	uint8_t ExtendedChecksum;
	uint8_t Reserved[3];
} __attribute__((packed)) rsdp_t;

uintptr_t find_mcfg_base(rsdp_t *rsdp);

#endif
