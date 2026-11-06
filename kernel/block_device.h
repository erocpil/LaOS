/*
 * block_device.h — 通用块设备接口
 *
 * 块设备提供按扇区读取的接口。驱动在初始化时注册设备，文件系统通过
 * block_read() 访问设备，无需依赖具体传输方式。
 */

#ifndef __BLOCK_DEVICE_H__
#define __BLOCK_DEVICE_H__

#include <stdint.h>

struct block_device {
	const char *name;     /* human-readable (e.g. "virtio-blk") */
	uint32_t sector_size; /* typically 512 */
	void *driver_data;    /* opaque: driver-private state */

	/* 从 sector 开始读取至多 count 个扇区到 buf；返回实际读取数，错误时返回负值。 */
	int (*read)(struct block_device *dev, uint64_t sector, void *buf, uint32_t count);
};

/* 注册块设备；成功返回 0，注册表已满时返回 -1。 */
int block_device_register(struct block_device *dev);

/* 获取索引为 n 的设备；索引越界时返回 NULL。 */
struct block_device *block_device_get(int index);

/* 返回已注册设备数量。 */
int block_device_count(void);

/* 在 BOOT_PHASE_MEMORY_READY 阶段清空注册表；后续注册从索引 0 开始。 */
void block_device_reset(void);

/* 从最近注册的设备读取；返回扇区数，读取失败或没有已注册设备时返回负值。 */
int block_read(uint64_t sector, void *buf, uint32_t count);

#endif /* __BLOCK_DEVICE_H__ */
