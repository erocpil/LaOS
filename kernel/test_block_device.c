/* test_block_device.c — block_device 单元测试 */
#include "test_block_device.h"
#include "block_device.h"
#include "selftest.h"
#include "printf.h"
#include "string.h"

static int g_ok;
static const uint8_t g_stub_sector[512] = {
	[0] = 0xAA,
	[1] = 0xBB,
	[511] = 0xFF,
};

static int stub_read(struct block_device *dev, uint64_t sector, void *buf, uint32_t count)
{
	(void)dev;

	if (sector >= 8) {
		return -1;
	}

	for (uint32_t i = 0; i < count; i++) {
		memcpy((uint8_t *)buf + i * 512, g_stub_sector, 512);
	}

	return (int)count;
}

static struct block_device g_d1 = {
	.name = "bd-stub",
	.sector_size = 512,
	.read = stub_read,
};

static bool bd_done(void)
{
	return true;
}

static bool bd_passed(void)
{
	return g_ok == 3;
}

static void bd_start(void)
{
	g_ok = 0;

	block_device_reset();

	if (block_device_register(&g_d1) != 0) {
		return;
	}

	struct block_device *d = block_device_get(block_device_count() - 1);
	if (!d) {
		return;
	}

	uint8_t buf[512];
	if (d->read(d, 0, buf, 1) != 1) {
		return;
	}

	if (buf[0] != 0xAA || buf[1] != 0xBB || buf[511] != 0xFF) {
		return;
	}

	g_ok++;
	kprintf("[test_block_device] 1/3 register+read ok\n");

	static struct block_device g_d2 = {
		.name = "bd-stub2",
		.sector_size = 512,
		.read = stub_read,
	};

	block_device_register(&g_d2);

	int n = block_device_count();
	if (n < 2) {
		return;
	}

	if (!block_device_get(n - 2) || !block_device_get(n - 1) ||
			block_device_get(n - 2) == block_device_get(n - 1) ||
			block_device_get(99) != NULL) {
		return;
	}

	g_ok++;
	kprintf("[test_block_device] 2/3 multi-device ok\n");

	struct block_device dummy = {
		.name = "d",
		.sector_size = 512,
		.read = stub_read,
	};

	int ret = 0;
	for (int i = block_device_count(); i < 8; i++) {
		ret = block_device_register(&dummy);
	}

	if (ret != 0) {
		return;
	}
	if (block_device_register(&dummy) != -1) {
		return;
	}

	g_ok++;
	kprintf("[test_block_device] 3/3 overflow ok\n");

	block_device_reset();
}

static const struct selftest bd_test = {
	.name = "block_device",
	.start = bd_start,
	.done = bd_done,
	.passed = bd_passed,
};

void test_block_device_init(void)
{
	if (selftest_register(&bd_test) < 0) {
		kprintf("[test_block_device] WARNING: register failed\n");
	}
}

bool test_block_device_run(void)
{
	bd_start();

	if (g_ok == 3) {
		kprintf("[test_block_device] PASSED (3/3)\n");
	} else {
		kprintf("[test_block_device] FAILED (%d/3)\n", g_ok);
	}

	return g_ok == 3;
}
