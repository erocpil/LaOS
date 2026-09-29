/*
 * virtio_pci.h — x86_64 virtio-pci 传输层定义
 *
 * Virtio 1.x PCI transport: 通过 PCI capabilities 定位
 * common/notify/ISR/device config 结构在 BAR 中的偏移。
 */

#ifndef __VIRTIO_PCI_H__
#define __VIRTIO_PCI_H__

#include <stdint.h>

/* ---- Virtio PCI 设备 ID ---- */
#define VIRTIO_PCI_VENDOR_ID         0x1AF4
#define VIRTIO_PCI_DEVICE_ID_BLOCK   0x1001   /* transitional */
#define VIRTIO_PCI_DEVICE_ID_BLOCK_MODERN 0x1042  /* non-transitional */

/* ---- PCI capability types (cfg_type field) ---- */
#define VIRTIO_PCI_CAP_COMMON_CFG     1
#define VIRTIO_PCI_CAP_NOTIFY_CFG     2
#define VIRTIO_PCI_CAP_ISR_CFG        3
#define VIRTIO_PCI_CAP_DEVICE_CFG     4
#define VIRTIO_PCI_CAP_PCI_CFG        5

/* ---- PCI capability structure (in config space) ---- */
struct virtio_pci_cap {
	uint8_t  cap_vndr;     /* 0x09 = vendor-specific */
	uint8_t  cap_next;
	uint8_t  cap_len;
	uint8_t  cfg_type;     /* 1=common, 2=notify, 3=ISR, 4=device, 5=PCI */
	uint8_t  bar;
	uint8_t  id;
	uint8_t  padding[2];
	uint32_t offset;       /* offset within BAR */
	uint32_t length;       /* length of structure */
} __attribute__((packed));

/* ---- Notify capability (extends virtio_pci_cap) ---- */
struct virtio_pci_notify_cap {
	struct virtio_pci_cap cap;
	uint32_t notify_off_multiplier;  /* queue_notify_off * multiplier = offset */
} __attribute__((packed));

/* ---- Common configuration structure (at BAR + offset) ---- */
struct virtio_pci_common_cfg {
	uint32_t device_feature_select;
	uint32_t device_feature;
	uint32_t driver_feature_select;
	uint32_t driver_feature;
	uint16_t msix_config;
	uint16_t num_queues;
	uint8_t  device_status;
	uint8_t  config_generation;

	uint16_t queue_select;
	uint16_t queue_size;
	uint16_t queue_msix_vector;
	uint16_t queue_enable;
	uint16_t queue_notify_off;
	uint64_t queue_desc;
	uint64_t queue_driver;
	uint64_t queue_device;
};

/* ---- 状态位 (same as MMIO) ---- */
#define VIRTIO_STATUS_ACKNOWLEDGE       1
#define VIRTIO_STATUS_DRIVER            2
#define VIRTIO_STATUS_FEATURES_OK       8
#define VIRTIO_STATUS_DRIVER_OK         4
#define VIRTIO_STATUS_FAILED            128

/* ---- 设备 ID ---- */
#define VIRTIO_ID_BLOCK                 2

/* ---- Virtqueue descriptor (same layout as MMIO) ---- */
#define VIRTQ_DESC_F_NEXT               1
#define VIRTQ_DESC_F_WRITE              2

struct virtq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct virtq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];
};

struct virtq_used_elem {
	uint32_t id;
	uint32_t len;
};

struct virtq_used {
	uint16_t flags;
	uint16_t idx;
	struct virtq_used_elem ring[];
};

#define VIRTQ_SIZE  8
#define VIRTIO_BLK_SECTOR_SIZE  512

/* ---- API ---- */
int virtio_pci_blk_init(void);

#endif /* __VIRTIO_PCI_H__ */
