/*
 * block_device.h — generic block device abstraction
 *
 * A block device provides sector-granularity read access.
 * Drivers register themselves at init time; consumers
 * (LaFS, future FAT, etc.) call block_read() without
 * knowing the underlying transport (virtio-mmio, virtio-pci, ATA).
 */

#ifndef __BLOCK_DEVICE_H__
#define __BLOCK_DEVICE_H__

#include <stdint.h>

struct block_device {
	const char *name;       /* human-readable (e.g. "virtio-blk") */
	uint32_t   sector_size; /* typically 512 */
	void      *driver_data; /* opaque: driver-private state */

	/* Read count sectors starting at 'sector' into 'buf'.
	 * Returns number of sectors read (<= count), <0 on error. */
	int (*read)(struct block_device *dev, uint64_t sector,
			void *buf, uint32_t count);
};

/* Register a block device.  Returns 0 on success, -1 if the
 * internal registry is full (max 8 devices). */
int block_device_register(struct block_device *dev);

/* Return the Nth registered device (0 = first).
 * Returns NULL if no such device. */
struct block_device *block_device_get(int index);

/* Return the number of registered devices. */
int block_device_count(void);

/* Clear the device registry — for unit test teardown.
 * After reset, block_device_register() starts from index 0. */
void block_device_reset(void);

/* Convenience: read from the last registered device.
 * Returns sectors read, <0 on error or no device. */
int block_read(uint64_t sector, void *buf, uint32_t count);

#endif /* __BLOCK_DEVICE_H__ */
