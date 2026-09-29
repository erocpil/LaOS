/*
 * pci.c - PCIe 配置空间访问与设备枚举
 *
 * 基于内存映射 IO(MMIO)的 PCIe 配置空间读写。
 * 使用 MCFG(ACPI 表)获取配置空间基址。
 */

#include "vmm.h"
#include "pci.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "export.h"
#include "debug.h"
#include "log.h"

#include "arch_irq.h"

/* ---- PCI INTx 路由表 ---- */

LIST_NODE(g_pci_devices);

/* ARM64 运行期 ECAM 配置：由 arch main.c 从 DTB 填充 */
uint64_t g_pcie_ecam_phys;
uint64_t g_pcie_ecam_size;
uint32_t g_pcie_bus_start;
uint32_t g_pcie_bus_end;

/* 运行期 ECAM 虚拟基址：pcie_init() 中根据架构设置 */
static uintptr_t g_pcie_virt_base;

struct pci_intx_route {
	uint32_t child_addr_hi;
	uint8_t pin;
	uint32_t irq_id;
};

#define PCI_INTX_ROUTE_MAX 32
static struct pci_intx_route g_pci_intx_routes[PCI_INTX_ROUTE_MAX];
static uint32_t g_pci_intx_route_count;
static uint32_t g_pci_intx_addr_mask = 0x1800;
static uint32_t g_pci_intx_pin_mask = 0x7;

void pci_set_intx_mask(uint32_t child_addr_hi, uint32_t pin)
{
	g_pci_intx_addr_mask = child_addr_hi;
	g_pci_intx_pin_mask = pin;
}

void pci_add_intx_route(uint32_t child_addr_hi, uint8_t pin, uint32_t irq_id)
{
	if (pin < 1 || pin > 4 || !irq_id ||
			g_pci_intx_route_count >= PCI_INTX_ROUTE_MAX) {
		return;
	}

	struct pci_intx_route *route =
		&g_pci_intx_routes[g_pci_intx_route_count++];
	route->child_addr_hi = child_addr_hi & g_pci_intx_addr_mask;
	route->pin = pin & g_pci_intx_pin_mask;
	route->irq_id = irq_id;
}

static uint32_t pci_resolve_intx(uint16_t bus, uint8_t slot,
		uint8_t func, uint8_t pin)
{
	uint32_t child_addr_hi = ((uint32_t)bus << 16) |
		((uint32_t)slot << 11) | ((uint32_t)func << 8);
	child_addr_hi &= g_pci_intx_addr_mask;
	pin &= g_pci_intx_pin_mask;
	for (uint32_t i = 0; i < g_pci_intx_route_count; i++) {
		struct pci_intx_route *route = &g_pci_intx_routes[i];
		if (route->child_addr_hi == child_addr_hi && route->pin == pin) {
			return route->irq_id;
		}
	}

	return 0;
}

int pci_enable_intx(struct pci_device *dev)
{
	if (!dev || dev->irq_pin < 1 || dev->irq_pin > 4 || !dev->irq_line) {
		return -1;
	}

	if (arch_pci_irq_enable(dev->irq_line) < 0) {
		return -1;
	}

	/* PCI command bit 10 disables legacy INTx when set. */
	uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
	pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND,
			cmd & ~(1U << 10));

	return 0;
}
EXPORT_SYMBOL(pci_enable_intx);

int pci_enable_msi(struct pci_device *dev)
{
	if (!dev || dev->msi_cap_off == 0) {
		return -1;
	}

	uint8_t cap = dev->msi_cap_off;
	uint16_t ctrl = pci_read16(dev->bus, dev->slot, dev->func,
			cap + PCI_MSI_CTRL);
	bool is64 = !!(ctrl & PCI_MSI_CTRL_64BIT);

	/* 分配 MSI 向量 (1 向量) */
	uint64_t msi_addr;
	uint32_t msi_data;

	if (arch_pci_msi_enable(dev->bus, dev->slot, dev->func,
			&msi_addr, &msi_data) != 0) {
		return -1;
	}

	/* 写 Message Address (32-bit low) */
	pci_write32(dev->bus, dev->slot, dev->func,
			cap + PCI_MSI_ADDR_LO, (uint32_t)msi_addr);

	if (is64) {
		/* 64-bit MSI: write high address + data */
		pci_write32(dev->bus, dev->slot, dev->func,
				cap + PCI_MSI_ADDR_HI, (uint32_t)(msi_addr >> 32));
		pci_write16(dev->bus, dev->slot, dev->func,
				cap + PCI_MSI_DATA_64, (uint16_t)msi_data);
	} else {
		/* 32-bit MSI */
		pci_write16(dev->bus, dev->slot, dev->func,
				cap + PCI_MSI_DATA_32, (uint16_t)msi_data);
	}

	/* 使能 MSI + 清除 INTx Disable */
	ctrl &= ~PCI_MSI_CTRL_MULTI_EN;
	ctrl |= PCI_MSI_CTRL_ENABLE;
	pci_write16(dev->bus, dev->slot, dev->func,
			cap + PCI_MSI_CTRL, ctrl);

	/* 禁用传统 INTx (PCI 2.3+ command bit 10) */
	uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func,
			PCI_REG_COMMAND);
	pci_write16(dev->bus, dev->slot, dev->func,
			PCI_REG_COMMAND, cmd | (1U << 10));

	kprintf("[PCI] MSI enabled: bus=%02x dev=%02x fn=%x "
			"addr=0x%llx data=0x%x\n",
			dev->bus, dev->slot, dev->func, msi_addr, msi_data);

	return 0;
}
EXPORT_SYMBOL(pci_enable_msi);

void pcie_init(uintptr_t mcfg_phys_base)
{
	// 获取当前内核页表
	uint64_t *kernel_pml4 = get_kernel_pagemap();

	// 映射标志:Present + Writable + Cache Disable + Write Through
	// MMIO 必须禁用缓存，否则读取 PCI 寄存器可能会拿到 CPU 缓存里的旧值
	uint64_t mmio_flags = PTE_PRESENT | PTE_WRITABLE |
		PTE_CACHE_DISABLE | PTE_WRITE_THROUGH;

	/*
	 * ARM64: ECAM 已在 identity 映射中，映射大小由 DTB 决定。
	 * x86_64: 需要 vmm_map_region()。
	 * 通过检查 g_pcie_ecam_size 是否为零来区分：
	 *   - 非零 = ARM64 identity mapping，跳过 vmm_map_region
	 *   - 零   = x86_64，使用原来的 vmm_map_region + PCIE_VIRT_BASE
	 */
	if (g_pcie_ecam_size == 0) {
		/* x86_64: HHDM 虚拟地址映射 */
		g_pcie_virt_base = PCIE_VIRT_BASE;
		if (vmm_map_region(kernel_pml4, PCIE_VIRT_BASE, mcfg_phys_base,
					256 * 1024 * 1024, mmio_flags) < 0) {
			panic("PCIe MMIO mapping failed");
		}
		L_TAG(LOG_PCI, "PCIe space mapped to %p (x86_64)\n",
				(void *)PCIE_VIRT_BASE);
	} else {
		/* ARM64: identity 映射，虚拟地址 = 物理地址 */
		g_pcie_virt_base = g_pcie_ecam_phys;
		L_TAG(LOG_PCI, "PCIe ECAM identity-mapped at 0x%llx, "
				"size %llu MB (ARM64)\n",
				(unsigned long long)g_pcie_ecam_phys,
				(unsigned long long)(g_pcie_ecam_size / (1024 * 1024)));
	}
}

/**
 * pci_get_config_addr() - 获取特定 PCI 设备的配置空间虚拟地址
 *
 * bus/dev/func: BDF 三要素。
 *
 * 返回值：配置空间寄存器的虚拟地址。计算公式：
 *   g_pcie_virt_base + (bus << 20) | (dev << 15) | (func << 12).
 *
 * 由于 PCIe 配置空间是内存映射的(ECAM)，直接通过指针操作。
 * 必须使用 volatile 关键字，防止编译器因为优化而跳过硬件寄存器的读取。
 * 获取特定 BDF (Bus, Device, Function) 的虚拟基地址
 * BDF 寻址：ECAM 的标准算法。
 *   每个 Bus 占用 1MB ($32 \	imes 8 \	imes 4096 = 1,048,576$ 字节)，左移 20 位。
 */
static inline void *pci_get_config_addr(uint8_t bus, uint8_t dev, uint8_t func)
{
	uintptr_t offset = ((uintptr_t)bus << 20) |
		((uintptr_t)dev << 15) | ((uintptr_t)func << 12);

	return (void*)(g_pcie_virt_base + offset);
}

/**
 * pci_read32() - 从 PCI 配置空间读取 32 位数据
 *
 * bus/dev/func: BDF 三要素。
 * offset: 寄存器偏移量(必须 4 字节对齐).
 *
 * 返回值：读取的 32 位值。
 */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
	volatile uint32_t* ptr = (volatile uint32_t*)((uintptr_t)
			pci_get_config_addr(bus, dev, func) + offset);

	return *ptr;
}
EXPORT_SYMBOL(pci_read32);

/** pci_read16() - 从 PCI 配置空间读取 16 位数据
 *
 * bus/dev/func: BDF 三要素。
 * offset: 寄存器偏移量(必须 2 字节对齐).
 *
 * 返回值：读取的 16 位值(常用于 VendorID,DeviceID 等).
 */
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
	volatile uint16_t* ptr = (volatile uint16_t*)((uintptr_t)
			pci_get_config_addr(bus, dev, func) + offset);

	return *ptr;
}
EXPORT_SYMBOL(pci_read16);

/** pci_read8() - 从 PCI 配置空间读取 8 位数据
 *
 * bus/dev/func: BDF 三要素。
 * offset: 寄存器偏移量。
 *
 * 返回值：读取的 8 位值(常用于 Class Code,Subclass 等).
 */
uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
	volatile uint8_t* ptr = (volatile uint8_t*)((uintptr_t)
			pci_get_config_addr(bus, dev, func) + offset);

	return *ptr;
}
EXPORT_SYMBOL(pci_read8);

/** pci_write32() - 向 PCI 配置空间写入 32 位数据
 *
 * bus/dev/func: BDF 三要素。
 * offset: 寄存器偏移量(必须 4 字节对齐).
 * value: 待写入值。
 */
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t value)
{
	// 获取设备基地址并加上偏移量
	volatile uint32_t* ptr = (volatile uint32_t*)((uintptr_t)pci_get_config_addr(bus, dev, func) + offset);

	// 执行写入操作
	*ptr = value;

	/* PCI config space 写入是 posted write，读回一次强制刷新，
	 * 确保后续依赖此次写入的操作（如 BAR 使能后的 MMIO 访问）看到生效的值。 */
	(void)*ptr;
}
EXPORT_SYMBOL(pci_write32);

/**
 * pci_write16() - 向 PCI 配置空间写入 16 位数据
 *
 * bus/dev/func: BDF 三要素。
 * offset: 寄存器偏移量(必须 2 字节对齐)。
 * value: 待写入值。
 */
void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint16_t value)
{
	volatile uint16_t* ptr = (volatile uint16_t*)((uintptr_t)
			pci_get_config_addr(bus, dev, func) + offset);

	*ptr = value;
}
EXPORT_SYMBOL(pci_write16);

/**
 * pci_write8() - 向 PCI 配置空间写入 8 位数据
 *
 * bus/dev/func: BDF 三要素。
 * offset: 寄存器偏移量。
 * value: 待写入值。
 */
void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint8_t value)
{
	volatile uint8_t* ptr = (volatile uint8_t*)((uintptr_t)
			pci_get_config_addr(bus, dev, func) + offset);

	*ptr = value;
}
EXPORT_SYMBOL(pci_write8);

/** pci_max_bars() - 根据 Header Type 返回 BAR 数量 */
static int pci_max_bars(uint8_t header_type)
{
	switch (header_type & 0x7F) {
		case 0: return 6;  /* Type 0: 标准 PCI 设备 */
		case 1: return 2;  /* Type 1: PCI-to-PCI 桥，只有 BAR0/BAR1 */
		case 2: return 1;  /* Type 2: CardBus 桥 */
		default: return 0;
	}
}

/** pci_init_bar() - 读取并解析 BAR
 *
 * 64-bit BAR 探测的正确流程（PCI 规范 §6.2.5.1）：
 *   1. 关闭地址译码（保存并清除 Command 的 IO/Mem Enable 位）
 *   2. 保存低 32 位 BAR 原值
 *   3. 保存高 32 位 BAR 原值（如果是 64 位 BAR）
 *   4. 向低 BAR 写入 0xFFFFFFFF，读取 mask
 *   5. 向高 BAR 写入 0xFFFFFFFF（64 位 BAR），读取高 mask
 *   6. 恢复高 BAR 原值（64 位 BAR，必须先于低 BAR）
 *   7. 恢复低 BAR 原值
 *   8. 恢复 Command 寄存器（重新启用地址译码）
 */
static void pci_init_bar(struct pci_device *dev, int bar_idx)
{
	uint32_t bar_reg = PCI_REG_BAR0 + (bar_idx * 4);
	uint32_t old_val = pci_read32(dev->bus, dev->slot, dev->func, bar_reg);

	/* 1. 关闭地址译码 */
	uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
	pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND,
			cmd & ~0x3);  /* 清除 IO Space + Memory Space Enable */

	bool is_64bit = ((old_val & 0x6) == 0x4);  /* Memory Space, 64-bit */
	uint32_t old_high = 0;
	uint32_t high_mask = 0;

	/* 2-3. 保存低/高 BAR 原值（译码关闭期间） */
	if (is_64bit) {
		old_high = pci_read32(dev->bus, dev->slot, dev->func, bar_reg + 4);
	}

	/* 4. 探测低 32 位 size mask */
	pci_write32(dev->bus, dev->slot, dev->func, bar_reg, 0xFFFFFFFF);
	uint32_t mask = pci_read32(dev->bus, dev->slot, dev->func, bar_reg);

	/* 5. 探测高 32 位 size mask（64 位 BAR）※译码仍关闭 */
	if (is_64bit) {
		pci_write32(dev->bus, dev->slot, dev->func, bar_reg + 4, 0xFFFFFFFF);
		high_mask = pci_read32(dev->bus, dev->slot, dev->func, bar_reg + 4);
	}

	/* 6. 恢复高 BAR（先高后低，PCI 规范要求） */
	if (is_64bit)
		pci_write32(dev->bus, dev->slot, dev->func, bar_reg + 4, old_high);

	/* 7. 恢复低 BAR */
	pci_write32(dev->bus, dev->slot, dev->func, bar_reg, old_val);

	/* 8. 恢复 Command 寄存器（重新启用地址译码） */
	pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, cmd);

	if (mask == 0 || mask == 0xFFFFFFFF) {
		return;
	}

	if (old_val & 0x1) { // IO Space
		dev->bar_type[bar_idx] = 1;
		dev->bar_phys[bar_idx] = old_val & ~0x3;
		dev->bar_size[bar_idx] = ~(mask & ~0x3) + 1;
	} else { // Memory Space
		if (is_64bit) {
			uint64_t full_mask = mask | ((uint64_t)high_mask << 32);
			if (full_mask == 0 || full_mask == 0xFFFFFFFFFFFFFFFFULL) {
				dev->bar_type[bar_idx] = 0;
				return;
			}

			dev->bar_type[bar_idx] = 0;
			dev->bar_phys[bar_idx] = (old_val & ~0xF) | ((uint64_t)old_high << 32);
			dev->bar_size[bar_idx] = ~(full_mask & ~0xF) + 1;
		} else {
			dev->bar_type[bar_idx] = 0;
			dev->bar_phys[bar_idx] = old_val & ~0xF;
			dev->bar_size[bar_idx] = ~(mask & ~0xF) + 1;
		}
	}
}

/** pci_scan_all_buses() - 遍历 256 条总线，并识别出网卡设备。 */
void pci_scan_all_buses(void)
{
	L("\n[PCI] Starting Enumeration...");
	L("\n%-7s | %-6s | %-6s | %-5s | %-3s | %-2s | %-4s",
			"BDF", "VENDER", "DEVICE", "CLASS", "REV", "IF", "TYPE");
	L("--------+--------+--------+-------+-----+----+------");

	for (uint16_t bus = g_pcie_bus_start; bus <= g_pcie_bus_end; bus++) {
		for (uint8_t dev = 0; dev < 32; dev++) {
			// 检查第一个 function 确定设备是否存在
			// 每一个设备至少有 Function 0
			uint16_t vendor = pci_read16(bus, dev, 0, PCI_REG_VENDOR_ID);

			// 0xFFFF 表示该槽位没有设备
			if (vendor == 0xFFFF) {
				continue;
			}

			// 检查该物理设备是否包含多个功能 (Multi-function Device)
			uint8_t header_type = pci_read8(bus, dev, 0, PCI_REG_HEADER_TYPE);
			// 多功能设备探测 (0x80 掩码)：Header Type 的最高位如果为 1，
			// 说明这个物理槽位上有多个逻辑功能（比如一个物理网卡有两个电口）
			// 如果不检查这一位，可能会漏掉很多设备。
			uint8_t func_count = (header_type & 0x80) ? 8 : 1;

			for (uint8_t func = 0; func < func_count; func++) {
				uint16_t v = pci_read16(bus, dev, func, PCI_REG_VENDOR_ID);
				if (v == 0xFFFF) {
					continue;
				}

				// 读取当前 function 自己的 Header Type
				uint8_t func_header = pci_read8(bus, dev, func, PCI_REG_HEADER_TYPE);

				// 1. 创建并填充设备结构体
				struct pci_device *pci_dev = kmalloc(sizeof(struct pci_device));
				if (!pci_dev) {
					L("Cannot kmalloc(struct pci_device)");
					return;
				}
				memset(pci_dev, 0, sizeof(*pci_dev));

				pci_dev->bus = bus;
				pci_dev->slot = dev;
				pci_dev->func = func;
				pci_dev->vendor_id = v;
				pci_dev->device_id = pci_read16(bus, dev, func, PCI_REG_DEVICE_ID);
				pci_dev->class_code = pci_read8(bus, dev, func, PCI_REG_CLASS_CODE);
				pci_dev->subclass = pci_read8(bus, dev, func, PCI_REG_SUBCLASS);
				// prog_if 用于标识该设备所遵循的具体寄存器级别接口标准。
				// Class/Subclass 告诉它是"什么"设备，比如：
				// 大类是存储控制器，子类是 SATA 控制器).
				// prog_if 告诉"怎么用"这个设备，比如：
				// 它遵循的是 IDE 接口，AHCI 接口 还是 NVMe 接口。
				// 通常在驱动程序加载阶段才发挥作用
				pci_dev->prog_if = pci_read8(bus, dev, func, PCI_REG_PROG_IF);
				pci_dev->rev = pci_read8(bus, dev, func, PCI_REG_REVISION);
				pci_dev->irq_line = pci_read8(bus, dev, func, PCI_REG_INTERRUPT_LINE);
				pci_dev->irq_pin = pci_read8(bus, dev, func, PCI_REG_INTERRUPT_PIN);
				uint32_t routed_irq = pci_resolve_intx(bus, dev, func,
						pci_dev->irq_pin);
				if (routed_irq) {
					pci_dev->irq_line = routed_irq;
					kprintf("[PCI] %02x:%02x.%x INT%c → IRQ %u\n",
							bus, dev, func, 'A' + pci_dev->irq_pin - 1,
							pci_dev->irq_line);
				}

				/* Scan MSI capability */
				pci_dev->msi_cap_off = 0;
				{
					uint8_t cap_ptr = pci_read8(bus, dev, func,
							PCI_REG_CAP_PTR);
					while (cap_ptr >= 0x40) {
						uint8_t cap_id = pci_read8(bus, dev, func,
								cap_ptr);
						if (cap_id == PCI_CAP_ID_MSI) {
							pci_dev->msi_cap_off = cap_ptr;
							kprintf("[PCI] %02x:%02x.%x MSI cap @ 0x%02x\n",
									bus, dev, func, cap_ptr);
							break;
						}
						cap_ptr = pci_read8(bus, dev, func,
								cap_ptr + 1);
					}
				}

				// 2. 解析所有 BAR 资源（数量取决于 Header Type）
				int max_bars = pci_max_bars(func_header);
				for (int i = 0; i < max_bars; i++) {
					pci_init_bar(pci_dev, i);
					// 如果是 64 位 BAR，跳过下一个槽位
					uint32_t bar_val = pci_read32(bus, dev, func, PCI_REG_BAR0 + (i * 4));
					if (!(bar_val & 0x1) && (bar_val & 0x6) == 0x4) {
						i++;
					}
				}

				// 3. 打印信息
				L("%02x:%02x.%x | %04x   | %04x   | %02x%02x  | %02x  | %02x | %02x",
						pci_dev->bus, pci_dev->slot, pci_dev->func, pci_dev->vendor_id,
						pci_dev->device_id, pci_dev->class_code, pci_dev->subclass,
						pci_dev->rev, pci_dev->prog_if, func_header & 0x7F);

				// 4. 加入全局链表
				list_add_tail(&pci_dev->node, &g_pci_devices);

				if (pci_dev->class_code == 0x0C && pci_dev->subclass == 0x03) {
					// 如果是 USB 控制器，打印接口类型
					kprintf("  >>> USB Controller (IF: 0x%02x) - ", pci_dev->prog_if);
					if (pci_dev->prog_if == 0x20) {
						L("EHCI");
					} else if (pci_dev->prog_if == 0x30) {
						L("XHCI");
					} else {
						L("Unknown");
					}
				} else if (pci_dev->class_code == 0x02) {
					// 特殊识别：网卡 (Class 02h)
					char *sc = NULL;
					if (pci_dev->subclass == 0x00) {
						sc = "Ethernet";
					} else if (pci_dev->subclass == 0x04) {
						sc = "Fiber Channel";
					} else {
						sc = "Other Net";
					}
					L_TAG(LOG_PCI, "Detected network controller: %s\n", sc);

					// 直接打印 pci_dev 中已探测的 BAR 信息（不再重复写硬件寄存器）
					for (int b = 0; b < max_bars; b++) {
						if (pci_dev->bar_size[b] == 0)
							continue;
						if (pci_dev->bar_type[b] == 1) {
							L("      BAR%d: IO   [0x%04lx]",
									b, pci_dev->bar_phys[b]);
						} else {
							L("      BAR%d: MEM  [0x%08lx] size: %lu KB",
									b, pci_dev->bar_phys[b],
									(unsigned long)(pci_dev->bar_size[b] / 1024));
						}
					}
				}

				// 特殊识别 PCI 桥接器 (Class 06h, Subclass 04h)
				if (pci_dev->class_code == 0x06 && pci_dev->subclass == 0x04) {
					L("  >>> PCI-to-PCI Bridge detected.");
				}
			}
		}
	}

	L_TAG(LOG_PCI, "%ld devices found and indexed. Enumeration Complete.\n",
			list_size(&g_pci_devices));
}

struct pci_device *pci_find_device(uint16_t vendor, uint16_t device)
{
	struct pci_device *dev;
	list_for_each_entry(dev, &g_pci_devices, node) {
		if (dev->vendor_id == vendor && dev->device_id == device) {
			return dev;
		}
	}

	return NULL;
}
EXPORT_SYMBOL(pci_find_device);

void pci_init(void)
{
	L_TAG(LOG_PCI, "Scanning buses ...\n");
	pci_scan_all_buses();
}
