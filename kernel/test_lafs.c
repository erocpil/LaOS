/*
 * test_lafs.c — LaFS 文件系统单元测试
 *
 * 使用内存 stub block_device 提供预构建 LaFS 镜像。
 */

#include "test_lafs.h"
#include "lafs.h"
#include "block_device.h"
#include "selftest.h"
#include "printf.h"
#include "string.h"

#define TEST_SECTORS 16
static uint8_t g_img[TEST_SECTORS * 512] __attribute__((aligned(16)));
static int g_pass;
static int g_fail;

static int lafs_stub_read(struct block_device *dev, uint64_t sector,
		void *buf, uint32_t count)
{
	(void)dev;

	if (sector + count > TEST_SECTORS) {
		return -1;
	}
	memcpy(buf, g_img + sector * 512, count * 512);

	return (int)count;
}

static struct block_device g_dev = {
	.name = "lafs-test",
	.sector_size = 512,
	.read = lafs_stub_read,
};

static void put32(uint32_t off, uint32_t v)
{
	g_img[off] = (uint8_t)v;
	g_img[off + 1] = (uint8_t)(v >> 8);
	g_img[off + 2] = (uint8_t)(v >> 16);
	g_img[off + 3] = (uint8_t)(v >> 24);
}

static void put_inode(uint32_t ino, uint8_t type, const char *name,
		uint32_t size, uint32_t blk0)
{
	uint32_t off = 512 + ino * 128;
	g_img[off] = type;
	uint32_t nl = 0;

	while (name[nl] && nl < 31) {
		nl++;
	}
	memcpy(g_img + off + 1, name, nl);
	put32(off + 33, size);
	put32(off + 37, blk0);
}

static uint32_t put_dirent(uint32_t sec, uint32_t pos,
		uint32_t ino, const char *name)
{
	uint32_t nl = 0;
	uint32_t base = sec * 512 + pos;

	while (name[nl]) {
		nl++;
	}
	put32(base, ino);
	g_img[base + 4] = (uint8_t)nl;
	memcpy(g_img + base + 5, name, nl);

	return pos + 5 + nl;
}

static void build_image(void)
{
	memset(g_img, 0, sizeof(g_img));
	memcpy(g_img, "LaFS\x01", 5);
	put32(5, 4);
	put32(9, 1);
	put32(13, 2);
	put32(17, 0);

	const char *motd = "Hello LaFS test!\n";
	const char *ver = "0.1.0-test\n";

	put_inode(0, 2, "", 0, 2);
	put_inode(1, 2, "etc", 0, 3);
	put_inode(2, 1, "motd", (uint32_t)strlen(motd), 4);
	put_inode(3, 1, "version", (uint32_t)strlen(ver), 5);

	uint32_t p = put_dirent(2, 0, 1, "etc");
	put32(512 + 0 * 128 + 33, p);
	p = put_dirent(3, 0, 2, "motd");
	p = put_dirent(3, p, 3, "version");
	put32(512 + 1 * 128 + 33, p);

	memcpy(g_img + 4 * 512, motd, strlen(motd));
	memcpy(g_img + 5 * 512, ver, strlen(ver));
}

/* selftest lifecycle */

static bool lfs_done(void)
{
	return true;
}

static bool lfs_passed(void)
{
	return g_fail == 0;
}

static void lfs_start(void)
{
	g_pass = 0;
	g_fail = 0;

	/* 1/4: bad magic */
	g_img[0] = 0;
	g_img[1] = 0;
	if (lafs_mount() == 0) {
		g_fail++;
		return;
	}

	memcpy(g_img, "LaFS\x01", 5);
	g_pass++;

	/* 2/4: mount + /etc/motd */
	if (lafs_mount() != 0) {
		g_fail++;
		return;
	}

	int ino = lafs_open("/etc/motd");
	if (ino < 0) {
		g_fail++;
		return;
	}
	char buf[64];
	int n = lafs_read(ino, buf, 0, sizeof(buf) - 1);
	if (n <= 0 || buf[0] != 'H' || buf[1] != 'e') {
		g_fail++;
		return;
	}
	g_pass++;

	/* 3/4: /etc/version */
	ino = lafs_open("/etc/version");
	if (ino < 0) {
		g_fail++;
		return;
	}
	n = lafs_read(ino, buf, 0, sizeof(buf) - 1);
	if (n <= 0 || buf[0] != '0') {
		g_fail++;
		return;
	}
	g_pass++;

	/* 4/4: readdir */
	n = lafs_readdir(0, buf, sizeof(buf));
	if (n <= 0) {
		g_fail++;
		return;
	}
	int found = 0;
	for (int i = 0; i + 2 < n; i++) {
		if (buf[i] == 'e' && buf[i + 1] == 't' && buf[i + 2] == 'c') {
			found = 1;
		}
	}
	if (!found) {
		g_fail++;
		return;
	}
	g_pass++;
}

static const struct selftest lfs_test = {
	.name   = "lafs",
	.start  = lfs_start,
	.done   = lfs_done,
	.passed = lfs_passed,
};

void test_lafs_init(void)
{
	if (selftest_register(&lfs_test) < 0) {
		kprintf("[test_lfs] WARNING: register failed\n");
	}
}

bool test_lafs_run(void)
{
	g_pass = 0;
	g_fail = 0;

	block_device_reset();
	build_image();
	block_device_register(&g_dev);
	lfs_start();
	block_device_reset();
	if (g_fail == 0) {
		kprintf("[test_lfs] PASSED (%d/%d)\n", g_pass, 4);
	} else {
		kprintf("[test_lfs] FAILED\n");
	}

	return g_fail == 0;
}
