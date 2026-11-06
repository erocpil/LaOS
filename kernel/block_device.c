/*
 * block_device.c — generic block device registry
 *
 * Fixed-capacity registry. Devices are registered during initialization and
 * remain registered for the lifetime of the kernel.
 */

#include "block_device.h"
#include "boot_phase.h"
#include <stddef.h>

#define MAX_BLOCK_DEVICES 8

static struct block_device *g_devices[MAX_BLOCK_DEVICES];
static int g_dev_count;

int block_device_register(struct block_device *dev)
{
	if (g_dev_count >= MAX_BLOCK_DEVICES) {
		return -1;
	}
	g_devices[g_dev_count++] = dev;

	return 0;
}

struct block_device *block_device_get(int index)
{
	if (index < 0 || index >= g_dev_count) {
		return NULL;
	}

	return g_devices[index];
}

int block_device_count(void)
{
	return g_dev_count;
}

void block_device_reset(void)
{
	boot_phase_require(BOOT_PHASE_MEMORY_READY);
	g_dev_count = 0;
}

int block_read(uint64_t sector, void *buf, uint32_t count)
{
	/* Later registrations take precedence, allowing a test device to replace
	 * the default device for subsequent reads. */
	struct block_device *dev = block_device_get(g_dev_count - 1);
	if (!dev) {
		return -1;
	}

	return dev->read(dev, sector, buf, count);
}
