/*
 * block_device.c — generic block device registry
 *
 * Simple fixed-size array registry.  Append-only: devices are
 * registered at init time and never removed.
 */

#include "block_device.h"
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
	g_dev_count = 0;
}

int block_read(uint64_t sector, void *buf, uint32_t count)
{
	/* Use the LAST registered device — later registrations
	 * (e.g. test stubs) override earlier ones for convenience. */
	struct block_device *dev = block_device_get(g_dev_count - 1);
	if (!dev) {
		return -1;
	}

	return dev->read(dev, sector, buf, count);
}
