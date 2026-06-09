/*
 * virtio_pci.c — x86_64 virtio-pci 块设备驱动
 *
 * QEMU -machine q35 -device virtio-blk-pci 提供 transitional 设备
 * (vendor 0x1AF4, device 0x1001)。使用 modern (1.x) PCI transport
 * 通过 capabilities 定位寄存器，轮询模式，单队列。
 */

#include <stdint.h>
#include <stddef.h>
#include "virtio_pci.h"
#include "block_device.h"
#include "pci.h"
#include "vmm.h"
#include "hhdm.h"
#include "heap.h"
#include "pmm.h"
#include "define.h"
#include "printf.h"
#include "string.h"

/* ---- 全局状态 ---- */

static volatile struct virtio_pci_common_cfg *g_common;
static volatile uint8_t                       *g_notify_base;
static volatile uint8_t                       *g_isr;
static uint32_t                                g_notify_off_mult;
static int                                     g_initialized;
static uint64_t                                g_vq_phys;    /* virtqueue page phys addr */

static struct virtq_desc  *g_desc;
static struct virtq_avail *g_avail;
static struct virtq_used  *g_used;
static uint16_t            g_next_avail;
static uint16_t            g_last_used;

/*
 * Virtqueue memory layout (one 4KB page, physically contiguous):
 *   offset 0:   desc[0..7]         (8 x 16 = 128 bytes)
 *   offset 128: avail ring         (32 bytes, padded)
 *   offset 160: used ring          (128 bytes, padded)
 *   offset 288: request buffer     (16 hdr + 512 data + 1 status = 529 bytes)
 * Total: 288 + 529 = 817 bytes → fits comfortably in 4KB
 */
#define VQ_DESC_OFF   0
#define VQ_AVAIL_OFF  128
#define VQ_USED_OFF   160
#define VQ_REQ_OFF    288

#define VQ_REQ_HDR_OFF    0
#define VQ_REQ_DATA_OFF   16
#define VQ_REQ_STATUS_OFF (16 + VIRTIO_BLK_SECTOR_SIZE)

static struct block_device g_bd;

/* ---- 前向声明 ---- */
static int virtio_pci_blk_bd_read(struct block_device *dev, uint64_t sector,
                                   void *buf, uint32_t count);

/* ---- 寄存器读写辅助 ---- */

static inline uint8_t common_read8(uint32_t offset)
{
	return *(volatile uint8_t *)((uintptr_t)g_common + offset);
}

static inline void common_write8(uint32_t offset, uint8_t val)
{
	*(volatile uint8_t *)((uintptr_t)g_common + offset) = val;
}

static inline uint16_t common_read16(uint32_t offset)
{
	return *(volatile uint16_t *)((uintptr_t)g_common + offset);
}

static inline void common_write16(uint32_t offset, uint16_t val)
{
	*(volatile uint16_t *)((uintptr_t)g_common + offset) = val;
}

static inline uint32_t common_read32(uint32_t offset)
{
	return *(volatile uint32_t *)((uintptr_t)g_common + offset);
}

static inline void common_write32(uint32_t offset, uint32_t val)
{
	*(volatile uint32_t *)((uintptr_t)g_common + offset) = val;
}

static inline uint64_t common_read64(uint32_t offset)
{
	return *(volatile uint64_t *)((uintptr_t)g_common + offset);
}

static inline void common_write64(uint32_t offset, uint64_t val)
{
	*(volatile uint64_t *)((uintptr_t)g_common + offset) = val;
}

/* ---- PCI 能力遍历 ---- */

/* pci_config_addr — compute ECAM address for BDF */
static inline void *pci_config_addr(uint8_t bus, uint8_t dev, uint8_t func)
{
	uintptr_t offset = ((uintptr_t)bus << 20) |
		((uintptr_t)dev << 15) | ((uintptr_t)func << 12);
	return (void *)(PCIE_VIRT_BASE + offset);
}

/*
 * pci_find_capability — 在 PCI 配置空间中扫描 capability 链表，
 * 查找 cfg_type 匹配的能力结构。返回 NULL 未找到。
 */
static const struct virtio_pci_cap *
pci_find_capability(struct pci_device *dev, uint8_t cfg_type,
		    uint8_t *out_cap_ptr)
{
	uint8_t ptr = pci_read8(dev->bus, dev->slot, dev->func, PCI_REG_CAP_PTR);

	while (ptr >= 0x40 && ptr <= 0xFC) {
		uint8_t cap_id = pci_read8(dev->bus, dev->slot, dev->func, ptr);
		if (cap_id != 0x09)  /* 0x09 = vendor-specific (virtio) */
			goto next;

		uint8_t type = pci_read8(dev->bus, dev->slot, dev->func, ptr + 3);

		if (type == cfg_type) {
			if (out_cap_ptr)
				*out_cap_ptr = ptr;
			/* Use rotating static buffer to avoid reuse */
			static struct virtio_pci_cap cap_buf[3];
			static int cap_idx;
			struct virtio_pci_cap *cap = &cap_buf[cap_idx++ % 3];
			memset(cap, 0, sizeof(*cap));
			cap->cap_vndr = pci_read8(dev->bus, dev->slot, dev->func, ptr);
			cap->cap_next = pci_read8(dev->bus, dev->slot, dev->func, ptr + 1);
			cap->cfg_type = pci_read8(dev->bus, dev->slot, dev->func, ptr + 3);
			cap->bar      = pci_read8(dev->bus, dev->slot, dev->func, ptr + 4);
			cap->offset   = pci_read32(dev->bus, dev->slot, dev->func, ptr + 8);
			cap->length   = pci_read32(dev->bus, dev->slot, dev->func, ptr + 12);
			return cap;
		}

	next:
		ptr = pci_read8(dev->bus, dev->slot, dev->func, ptr + 1);
	}
	return NULL;
}

/* ---- BAR 映射 ---- */

#define BAR_MMIO_VBASE 0xFFFFD00000000000ULL
static uint64_t g_bar_mmio_next = BAR_MMIO_VBASE;

/*
 * map_bar — 将 PCI BAR 通过 vmm_map_region 映射到虚拟地址空间。
 * PCI MMIO 区域通常不在 HHDM 覆盖范围（特别是 64-bit BAR 高位），
 * 需要显式页表映射。返回虚拟地址，失败返回 NULL。
 */
static void *map_bar(struct pci_device *dev, int bar_idx, uint64_t offset,
		     uint64_t length)
{
	if (bar_idx >= PCI_MAX_BARS || dev->bar_size[bar_idx] == 0)
		return NULL;

	uint64_t phys = dev->bar_phys[bar_idx];
	if (offset + length > dev->bar_size[bar_idx]) {
		kprintf("[virtio-pci] BAR%d offset+len out of range\r\n", bar_idx);
		return NULL;
	}

	/* Map the entire BAR page-aligned */
	uint64_t map_phys = (phys + offset) & ~0xFFFULL;
	uint64_t map_len  = ((phys + offset + length + 0xFFF) & ~0xFFFULL) - map_phys;

	uint64_t vaddr = g_bar_mmio_next;
	g_bar_mmio_next += map_len;
	g_bar_mmio_next = (g_bar_mmio_next + 0xFFF) & ~0xFFFULL;

	uint64_t *pml4 = get_kernel_pagemap();
	uint64_t mmio_flags = PTE_PRESENT | PTE_WRITABLE |
		PTE_CACHE_DISABLE | PTE_WRITE_THROUGH;

	if (vmm_map_region(pml4, vaddr, map_phys, map_len, mmio_flags) < 0) {
		kprintf("[virtio-pci] BAR%d vmm_map_region failed\r\n", bar_idx);
		return NULL;
	}

	return (void *)(uintptr_t)(vaddr + ((phys + offset) - map_phys));
}

/* ---- 初始化 ---- */

int virtio_pci_blk_init(void)
{
	if (g_initialized)
		return 0;

	/* 1. 查找 virtio-blk PCI 设备 */
	struct pci_device *blk_dev = NULL;
	{
		struct pci_device *d;
		list_for_each_entry(d, &g_pci_devices, node) {
			if (d->vendor_id == VIRTIO_PCI_VENDOR_ID &&
			    (d->device_id == VIRTIO_PCI_DEVICE_ID_BLOCK ||
			     d->device_id == VIRTIO_PCI_DEVICE_ID_BLOCK_MODERN)) {
				blk_dev = d;
				break;
			}
		}
	}

	if (!blk_dev) {
		kprintf("[virtio-pci] no virtio-blk device found\r\n");
		return -1;
	}

	kprintf("[virtio-pci] found %02x:%02x.%x vendor=0x%04x device=0x%04x\r\n",
		blk_dev->bus, blk_dev->slot, blk_dev->func,
		blk_dev->vendor_id, blk_dev->device_id);

	/* 2. 使能 bus mastering + MMIO */
	{
		uint16_t cmd = pci_read16(blk_dev->bus, blk_dev->slot,
					  blk_dev->func, PCI_REG_COMMAND);
		cmd |= 0x4;  /* bus master */
		cmd |= 0x2;  /* memory space */
		pci_write16(blk_dev->bus, blk_dev->slot, blk_dev->func,
			    PCI_REG_COMMAND, cmd);
	}

	/* 3. 解析 PCI capabilities */
	uint8_t notify_cap_ptr = 0;
	const struct virtio_pci_cap *common_cap =
		pci_find_capability(blk_dev, VIRTIO_PCI_CAP_COMMON_CFG, NULL);
	const struct virtio_pci_cap *notify_cap =
		pci_find_capability(blk_dev, VIRTIO_PCI_CAP_NOTIFY_CFG,
				    &notify_cap_ptr);
	const struct virtio_pci_cap *isr_cap =
		pci_find_capability(blk_dev, VIRTIO_PCI_CAP_ISR_CFG, NULL);

	if (!common_cap || !notify_cap || !isr_cap) {
		kprintf("[virtio-pci] missing capabilities: common=%p notify=%p isr=%p\r\n",
			(void*)common_cap, (void*)notify_cap, (void*)isr_cap);
		return -1;
	}

	/* 4. 映射 BAR 区域 */
	g_common = map_bar(blk_dev, common_cap->bar, common_cap->offset,
			   common_cap->length);
	g_isr    = map_bar(blk_dev, isr_cap->bar, isr_cap->offset,
			   isr_cap->length);

	/* notify 能力有扩展字段（notify_off_multiplier 在 cap 结构体偏移 16） */
	{
		g_notify_base = map_bar(blk_dev, notify_cap->bar,
					notify_cap->offset, notify_cap->length);
		g_notify_off_mult = pci_read32(blk_dev->bus, blk_dev->slot,
					       blk_dev->func,
					       notify_cap_ptr + 16);
	}

	if (!g_common || !g_isr || !g_notify_base) {
		kprintf("[virtio-pci] BAR mapping failed\r\n");
		return -1;
	}

	kprintf("[virtio-pci] common@BAR%d+0x%x notify@BAR%d+0x%x mult=%u isr@BAR%d+0x%x\r\n",
		common_cap->bar, common_cap->offset,
		notify_cap->bar, notify_cap->offset, g_notify_off_mult,
		isr_cap->bar, isr_cap->offset);

	/* 5. Reset device */
	common_write8(offsetof(struct virtio_pci_common_cfg, device_status), 0);
	/* read-back for MMIO flush */
	(void)common_read8(offsetof(struct virtio_pci_common_cfg, device_status));

	/* 6. ACKNOWLEDGE → DRIVER */
	common_write8(offsetof(struct virtio_pci_common_cfg, device_status),
		      VIRTIO_STATUS_ACKNOWLEDGE);
	common_write8(offsetof(struct virtio_pci_common_cfg, device_status),
		      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

	/* 7. Negotiate features — accept VIRTIO_F_VERSION_1 if offered */
	common_write32(offsetof(struct virtio_pci_common_cfg, device_feature_select), 0);
	(void)common_read32(offsetof(struct virtio_pci_common_cfg, device_feature));
	common_write32(offsetof(struct virtio_pci_common_cfg, device_feature_select), 1);
	uint32_t features_hi = common_read32(
		offsetof(struct virtio_pci_common_cfg, device_feature));
	if (features_hi & (1u << 0)) {  /* VIRTIO_F_VERSION_1 = bit 32 */
		common_write32(offsetof(struct virtio_pci_common_cfg,
				       driver_feature_select), 1);
		common_write32(offsetof(struct virtio_pci_common_cfg,
				       driver_feature), (1u << 0));
		kprintf("[virtio-pci] negotiated VIRTIO_F_VERSION_1\r\n");
	}
	common_write32(offsetof(struct virtio_pci_common_cfg,
			       driver_feature_select), 0);
	common_write32(offsetof(struct virtio_pci_common_cfg, driver_feature), 0);

	common_write8(offsetof(struct virtio_pci_common_cfg, device_status),
		      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
		      VIRTIO_STATUS_FEATURES_OK);
	/* Verify device accepted FEATURES_OK */
	{
		uint8_t status = common_read8(
			offsetof(struct virtio_pci_common_cfg, device_status));
		if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
			kprintf("[virtio-pci] device rejected FEATURES_OK "
				"(status=0x%x)\r\n", status);
			common_write8(offsetof(struct virtio_pci_common_cfg,
					      device_status),
				      VIRTIO_STATUS_ACKNOWLEDGE |
				      VIRTIO_STATUS_DRIVER |
				      VIRTIO_STATUS_FAILED);
			return -1;
		}
	}

	/* 8. Setup queue 0 */
	common_write16(offsetof(struct virtio_pci_common_cfg, queue_select), 0);
	uint16_t qmax = common_read16(
		offsetof(struct virtio_pci_common_cfg, queue_size));
	if (qmax == 0) {
		kprintf("[virtio-pci] queue 0 size is 0\r\n");
		return -1;
	}
	uint16_t qnum = (VIRTQ_SIZE < qmax) ? VIRTQ_SIZE : qmax;
	common_write16(offsetof(struct virtio_pci_common_cfg, queue_size), qnum);

	/*
	 * Allocate one physically contiguous page for the entire virtqueue +
	 * request buffer.  pmm_alloc returns a physical address; the page is
	 * accessible via HHDM (phys_to_virt).  This guarantees both physical
	 * contiguity and correct virt_to_phys() translation for DMA addresses.
	 */
	void *vq_page = pmm_alloc();
	if (!vq_page) {
		kprintf("[virtio-pci] pmm_alloc vq page failed\r\n");
		return -1;
	}
	g_vq_phys = (uint64_t)(uintptr_t)vq_page;
	uint8_t *vq_mem = phys_to_virt(g_vq_phys);
	memset(vq_mem, 0, PAGE_SIZE);

	g_desc  = (struct virtq_desc  *)(vq_mem + VQ_DESC_OFF);
	g_avail = (struct virtq_avail *)(vq_mem + VQ_AVAIL_OFF);
	g_used  = (struct virtq_used  *)(vq_mem + VQ_USED_OFF);

	/* Write virtqueue addresses (physical, for device DMA) */
	common_write64(offsetof(struct virtio_pci_common_cfg, queue_desc),
		       g_vq_phys + VQ_DESC_OFF);
	common_write64(offsetof(struct virtio_pci_common_cfg, queue_driver),
		       g_vq_phys + VQ_AVAIL_OFF);
	common_write64(offsetof(struct virtio_pci_common_cfg, queue_device),
		       g_vq_phys + VQ_USED_OFF);

	/* Enable queue */
	common_write16(offsetof(struct virtio_pci_common_cfg, queue_enable), 1);

	/* 9. DRIVER_OK */
	common_write8(offsetof(struct virtio_pci_common_cfg, device_status),
		      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
		      VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

	g_next_avail  = 0;
	g_last_used   = 0;
	g_initialized = 1;

	/* 10. Register with block_device */
	g_bd.name        = "virtio-pci-blk";
	g_bd.sector_size = VIRTIO_BLK_SECTOR_SIZE;
	g_bd.driver_data = NULL;
	g_bd.read        = virtio_pci_blk_bd_read;
	block_device_register(&g_bd);

	kprintf("[virtio-pci] ready qnum=%u\r\n", qnum);
	return 0;
}

/* ---- 轮询 ---- */

/*
 * Poll for used-ring update.  Returns 0 on completion, -1 on timeout.
 */
static int virtio_pci_blk_poll(void)
{
	uint64_t timeout = 10000000;
	while (g_used->idx == g_last_used && timeout--) {
		__asm__ volatile("pause" ::: "memory");
	}
	if (g_used->idx == g_last_used)
		return -1;  /* timeout */
	return 0;
}

/* ---- block_device wrapper ---- */

static int virtio_pci_blk_bd_read(struct block_device *dev, uint64_t sector,
                                   void *buf, uint32_t count)
{
	(void)dev;

	if (!g_initialized) return -1;
	if (count == 0) return 0;

	uint8_t *vq_mem = phys_to_virt(g_vq_phys);

	uint32_t done = 0;
	while (done < count) {
		/*
		 * Compute physical addresses within the shared vq page.
		 * All descriptor addresses are (g_vq_phys + offset) so
		 * the device sees correct physical addresses for DMA.
		 */
		uint64_t req_phys    = g_vq_phys + VQ_REQ_OFF;
		uint64_t hdr_phys    = req_phys + VQ_REQ_HDR_OFF;
		uint64_t data_phys   = req_phys + VQ_REQ_DATA_OFF;
		uint64_t status_phys = req_phys + VQ_REQ_STATUS_OFF;

		uint8_t *req     = vq_mem + VQ_REQ_OFF;
		uint8_t *hdr     = req + VQ_REQ_HDR_OFF;
		uint8_t *bounce  = req + VQ_REQ_DATA_OFF;
		uint8_t *status  = req + VQ_REQ_STATUS_OFF;

		/* Build request header */
		uint32_t *hdr32 = (uint32_t *)hdr;
		hdr32[0] = 0;          /* type = read */
		hdr32[1] = 0;          /* reserved */
		*(uint64_t *)&hdr32[2] = sector + done;

		/* Descriptor chain: hdr → data → status */
		uint16_t desc_head = 0;
		g_desc[0].addr  = hdr_phys;
		g_desc[0].len   = 16;
		g_desc[0].flags = VIRTQ_DESC_F_NEXT;
		g_desc[0].next  = 1;

		g_desc[1].addr  = data_phys;
		g_desc[1].len   = VIRTIO_BLK_SECTOR_SIZE;
		g_desc[1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
		g_desc[1].next  = 2;

		/* Set status to sentinel so stale data isn't mistaken for OK */
		*status = 0xFF;
		g_desc[2].addr  = status_phys;
		g_desc[2].len   = 1;
		g_desc[2].flags = VIRTQ_DESC_F_WRITE;
		g_desc[2].next  = 0;

		g_avail->ring[g_next_avail % VIRTQ_SIZE] = desc_head;
		g_next_avail++;
		/* memory barrier before updating avail->idx */
		__asm__ volatile("" ::: "memory");
		g_avail->idx = g_next_avail;
		__asm__ volatile("" ::: "memory");

		/* Notify device */
		uint16_t notify_off = common_read16(
			offsetof(struct virtio_pci_common_cfg, queue_notify_off));
		uint64_t notify_addr = (uint64_t)(uintptr_t)g_notify_base +
			notify_off * g_notify_off_mult;
		*(volatile uint16_t *)notify_addr = 0;

		/* Wait for completion */
		if (virtio_pci_blk_poll() != 0) {
			kprintf("[virtio-pci] poll timeout sector=%llu\r\n",
				sector + done);
			return done > 0 ? (int)done : -1;
		}

		/* Verify status was updated by device */
		if (*status == 0xFF) {
			kprintf("[virtio-pci] status not updated sector=%llu\r\n",
				sector + done);
			return done > 0 ? (int)done : -1;
		}

		if (*status != 0) {
			kprintf("[virtio-pci] read err sector=%llu status=%u\r\n",
				sector + done, *status);
			return done > 0 ? (int)done : -1;
		}

		/* Copy bounce buffer → caller's buffer */
		memcpy((uint8_t *)buf + done * VIRTIO_BLK_SECTOR_SIZE, bounce,
		       VIRTIO_BLK_SECTOR_SIZE);

		g_last_used++;
		done++;
	}

	return (int)done;
}
