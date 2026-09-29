#ifndef __PCI_H__
#define __PCI_H__

/*
 * pci.h - PCIe 配置空间访问
 */

#include <stdint.h>
#include <stdbool.h>

#include "list.h"

#define PCI_REG_VENDOR_ID		0x00
#define PCI_REG_DEVICE_ID   	0x02
#define PCI_REG_COMMAND     	0x04
#define PCI_REG_STATUS      	0x06
#define PCI_REG_REVISION    	0x08
#define PCI_REG_CLASS_CODE  	0x0B
#define PCI_REG_SUBCLASS    	0x0A
#define PCI_REG_PROG_IF     	0x09
#define PCI_REG_HEADER_TYPE 	0x0E
#define PCI_REG_BAR0        	0x10
#define PCI_REG_INTERRUPT_LINE	0x3C
#define PCI_REG_INTERRUPT_PIN	0x3D
#define PCI_REG_CAP_PTR         0x34

/* PCI Capability IDs */
#define PCI_CAP_ID_MSI          0x05
#define PCI_CAP_ID_MSIX         0x11

/* MSI Capability 偏移 (相对于 capability 起始) */
#define PCI_MSI_CTRL            0x02 /* Message Control (16-bit) */
#define PCI_MSI_ADDR_LO         0x04 /* Message Address low (32-bit, 64-bit capable) */
#define PCI_MSI_ADDR_HI         0x08 /* Message Address high (32-bit) */
#define PCI_MSI_DATA_32         0x08 /* Message Data (32-bit address mode) */
#define PCI_MSI_DATA_64         0x0C /* Message Data (64-bit address mode) */
#define PCI_MSI_MASK_32         0x0C /* Mask Bits (32-bit, optional) */
#define PCI_MSI_MASK_64         0x10 /* Mask Bits (64-bit, optional) */

/* MSI Control 位 */
#define PCI_MSI_CTRL_ENABLE    (1 << 0)
#define PCI_MSI_CTRL_MULTI_CAP (0x7 << 1) /* Multiple Message Capable */
#define PCI_MSI_CTRL_MULTI_EN  (0x7 << 4) /* Multiple Message Enable */
#define PCI_MSI_CTRL_64BIT     (1 << 7) /* 64-bit address capable */

// 定义 PCIe 虚拟基地址（允许 arch 层 override）
#ifndef PCIE_VIRT_BASE
#define PCIE_VIRT_BASE 0xFFFFC00000000000
#endif

#define PCI_MAX_BARS 6

/* ARM64 运行期 ECAM 配置（由 arch main.c 从 DTB 填充） */
extern uint64_t g_pcie_ecam_phys;
extern uint64_t g_pcie_ecam_size;
extern uint32_t g_pcie_bus_start;
extern uint32_t g_pcie_bus_end;

extern struct list_node g_pci_devices;

// 扫描阶段产出的结果，存放在 global_pci_devices 链表中
struct pci_device {
	uint16_t bus;
	uint16_t slot;
	uint16_t func;
	uint16_t vendor_id;
	uint16_t device_id;
	uint8_t class_code;
	uint8_t subclass;
	uint8_t prog_if;
	uint8_t rev;

	uintptr_t bar_phys[PCI_MAX_BARS];
	uint64_t bar_size[PCI_MAX_BARS];
	uint8_t   bar_type[PCI_MAX_BARS]; // 0: Memory, 1: IO
	uint32_t irq_line;
	uint8_t  irq_pin;
	uint8_t  msi_cap_off;      /* MSI capability offset (0 = none) */

	struct pci_driver *driver; // 指向匹配成功的驱动
	void *priv_data;           // 驱动私有数据(如 e1000_nic_t 结构体)
	struct list_node node;
};

// 内核加载模块后注册的元数据
struct pci_id {
	uint16_t vendor_id;
	uint16_t device_id;
};

struct pci_driver {
	const char *name;
	struct pci_id *id_table; // 支持的硬件列表，以 {0, 0} 结尾

	// 核心回调函数
	int (*probe)(struct pci_device *dev); // 硬件匹配成功后调用
	void (*remove)(struct pci_device *dev);

	struct list_node node;
};

void pcie_init(uintptr_t mcfg_phys_base);
void pci_scan_all_buses(void);
void pci_init(void);
void pci_set_intx_mask(uint32_t child_addr_hi, uint32_t pin);
void pci_add_intx_route(uint32_t child_addr_hi, uint8_t pin, uint32_t irq_id);
int pci_enable_intx(struct pci_device *dev);
int pci_enable_msi(struct pci_device *dev);
struct pci_device *pci_find_device(uint16_t vendor, uint16_t device);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t value);
void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint16_t value);
void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint8_t value);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);

#endif
