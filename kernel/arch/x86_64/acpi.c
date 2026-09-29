/*
 * acpi.c - ACPI RSDT/XSDT 表解析
 *
 * 在 boot 阶段扫描 ACPI 表，查找 MCFG(PCIe MMIO 基地址)等结构。
 * 当前仅用于 e1000 驱动获取 PCIe 配置空间。
 */

#include <stddef.h>
#include "acpi.h"
#include "string.h"
#include "hhdm.h"
#include "debug.h"

/**
 * acpi_validate_checksum() - 验证 ACPI 表的完整性。
 *
 * ACPI 规范要求每张表(header + body)的所有字节相加(uint8_t wrap-around)
 * 结果必须为 0.校验和由固件在写表时计算并填入最后一个字节使总和归零。
 *
 * 损坏的 ACPI 表可能导致：
 *   - 野指针：表内的物理地址字段(如 MCFG base address)是随机值
 *   - 越界读:Length 字段被破坏，遍历条目时读出界内存
 *   - 静默错误：表内容部分翻转，读到的设备信息全错
 *
 * 返回值:1 = checksum 正确，0 = 校验失败(表已损坏).
 */
int acpi_validate_checksum(acpi_header_t *header)
{
	uint8_t sum = 0;
	uint8_t *data = (uint8_t*)header;

	for (uint32_t i = 0; i < header->Length; i++) {
		sum += data[i];
	}

	return sum == 0;
}

/**
 * find_mcfg_base() - 在 64 位内核中，优先使用 XSDT(支持 64 位物理地址)。
 *
 * 调用 acpi_validate_checksum 验证根表和各条目表，ACPI 规范
 * 要求系统固件在写表时保证校验和，但实际硬件上固件 bug / 内存
 * 位翻转都可能破坏表内容。校验可以防止因坏表导致的野指针 panic。
 */
uintptr_t find_mcfg_base(rsdp_t *rsdp)
{
	acpi_header_t *root_table = NULL;
	uint32_t entry_count = 0;
	uint8_t entry_size = 0;

	L("[ACPI] RSDP Revision: %d", rsdp->Revision);

	// 1. 确定根表类型 (RSDT vs XSDT)
	if (rsdp->Revision >= 2 && rsdp->XsdtAddress != 0) {
		root_table = (acpi_header_t*)phys_to_virt(rsdp->XsdtAddress);
		entry_size = 8; // XSDT 使用 64 位指针
		L("[ACPI] Using XSDT at 0x%p (Phys: 0x%lx)", root_table, rsdp->XsdtAddress);
	} else {
		root_table = (acpi_header_t*)phys_to_virt((uintptr_t)rsdp->RsdtAddress);
		entry_size = 4; // RSDT 使用 32 位指针
		L("[ACPI] Using RSDT at 0x%p (Phys: 0x%x)", root_table, rsdp->RsdtAddress);
	}

	if (!root_table) {
		panic("[ACPI] ERROR: Root table is NULL!");
		return 0;
	}

	/*
	 * 验证根表校验和。
	 *
	 * 根表的 Length 字段位于 header 内，acpi_validate_checksum 用
	 * header->Length 确定遍历范围。若 Length 本身被破坏，for 循环
	 * 可能溢出----但这种情况意味着表已彻底损坏，遍历的后果(越界读 /
	 * 很快 sum ！= 0)仍然是安全的。
	 */
	if (!acpi_validate_checksum(root_table)) {
		L("[ACPI] ERROR: Root table checksum mismatch! Table may be corrupted.");
		return 0;
	}

	// 2. 计算表项数量
	entry_count = (root_table->Length - sizeof(acpi_header_t)) / entry_size;
	L("[ACPI] Scanning %d table entries ...", entry_count);

	// 3. 遍历寻找 MCFG
	uintptr_t mcfg_addr = 0;
	for (uint32_t i = 0; i < entry_count; i++) {
		uintptr_t table_phys = 0;

		if (entry_size == 8) {
			table_phys = ((uint64_t*)((uintptr_t)root_table +
						sizeof(acpi_header_t)))[i];
		} else {
			table_phys = ((uint32_t*)((uintptr_t)root_table +
						sizeof(acpi_header_t)))[i];
		}

		acpi_header_t *header = (acpi_header_t*)phys_to_virt(table_phys);

		// 打印每个找到的表名，方便调试
		char sig[5] = {0};
		memcpy(sig, header->Signature, 4);
		L("  - Found Table: [%s] at Phys: 0x%lx", sig, table_phys);

		/*
		 * 验证每张表的校验和。
		 *
		 * 即使根表校验正确，个别条目表仍可能损坏(固件 bug 只影响
		 * 部分表，或内存位翻转仅限于该表区域).逐表验证是每表 O(n)
		 * 的开销，但 ACPI 表通常不超过几十 KB，boot 期完全可接受。
		 */
		if (!acpi_validate_checksum(header)) {
			L("[ACPI] WARNING: Table [%s] checksum mismatch, skipping.", sig);
			continue;
		}

		if (memcmp(header->Signature, "MCFG", 4) == 0) {
			mcfg_addr = (uintptr_t)header;
			L("[ACPI] SUCCESS: Found MCFG at 0x%p", mcfg_addr);
		}
	}

	if (!mcfg_addr) {
		L("[ACPI] ERROR: MCFG table not found in ACPI!");
		return 0;
	}

	// 4. 解析 MCFG 条目
	mcfg_table_t *mcfg = (mcfg_table_t*)mcfg_addr;
	uint32_t mcfg_entries = (mcfg->Header.Length -
			sizeof(acpi_header_t) - 8) / sizeof(mcfg_entry_t);

	L("[MCFG] Table has %d entries.\n", mcfg_entries);

	for (uint32_t i = 0; i < mcfg_entries; i++) {
		mcfg_entry_t *entry = &mcfg->Entries[i];
		L("[MCFG] Entry %d:", i);
		L("       Base Address: 0x%lx", entry->BaseAddress);
		L("       Segment Group: %d", entry->PciSegmentGroup);
		L("       Bus Range: %d to %d", entry->StartBusNumber, entry->EndBusNumber);

		// 对于高性能网络，通常只返回第一个段的基地址
		if (i == 0) {
			return (uintptr_t)entry->BaseAddress;
		}
	}

	return 0;
}
