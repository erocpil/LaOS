/*
 * lafs.c — LaFS minimal read-only filesystem implementation
 *
 * Builds on virtio_blk for raw sector I/O.
 * Allocation: static BSS (no kmalloc), single-sector buffer.
 */

#include <stdint.h>
#include <stddef.h>
#include "lafs.h"
#include "block_device.h"
#include "printf.h"
#include "string.h"

/* ---- Superblock fields ---- */
static uint32_t g_inode_count;
static uint32_t g_inode_start;
static uint32_t g_data_start;
static int g_mounted;

/* One-sector read buffer (512 bytes). Reused across calls. */
static uint8_t g_sec[512] __attribute__((aligned(16)));

/* Helpers */

static int read_sector(uint32_t sec)
{
	int n = block_read(sec, g_sec, 1);
	return (n == 1) ? 0 : -1;
}

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
	return *(const uint32_t *)(p + off);
}

/**
 * read_inode_raw() - Read an inode from disk into g_sec (trashes sector buffer).
 *
 * Returns pointer to inode data inside g_sec, or NULL on error. */
static const uint8_t *read_inode_raw(uint32_t ino)
{
	if (ino >= g_inode_count)
		return NULL;
	uint32_t byte_off = ino * LAFS_INODE_SIZE;
	uint32_t sec = g_inode_start + byte_off / 512;
	if (read_sector(sec) != 0)
		return NULL;
	return g_sec + (byte_off % 512);
}

/* Mount */

int lafs_mount(void)
{
	if (read_sector(0) != 0) {
		return -1;
	}

	uint32_t magic = rd32(g_sec, 0);
	if (magic != LAFS_MAGIC) {
		kprintf("[lafs] bad magic 0x%08x, expected 0x%08x\r\n",
			magic, LAFS_MAGIC);
		return -1;
	}

	g_inode_count = rd32(g_sec, 5);
	g_inode_start = rd32(g_sec, 9);
	g_data_start = rd32(g_sec, 13);
	g_mounted = 1;

	kprintf("[lafs] mounted: %u inodes, table@%u, data@%u\r\n",
		g_inode_count, g_inode_start, g_data_start);

	return 0;
}

/* Path resolution */

/**
 * dir_lookup() - Look up a name in a directory inode's data blocks.
 *
 * Returns target inode index, or -1 if not found. */
static int dir_lookup(uint32_t dir_ino, const char *name, uint32_t name_len)
{
	const uint8_t *raw = read_inode_raw(dir_ino);
	if (!raw || raw[0] != 2) {
		return -1;
	}

	uint32_t dir_size = rd32(raw, 33);
	if (dir_size == 0) {
		return -1;
	}

	/* Read each data block and scan for entries */
	for (uint32_t off = 0; off < dir_size; ) {
		uint32_t blk_idx = off / 512;
		if (blk_idx >= LAFS_BLOCKS_MAX) {
			break;
		}
		uint32_t blk_sec = rd32(raw, 37 + blk_idx * 4);
		if (blk_sec == 0) {
			break;
		}

		if (read_sector(blk_sec) != 0) {
			return -1;
		}

		/* Scan entries within this sector */
		uint32_t pos = off % 512;
		while (pos + 5 <= 512) {
			/* Check if we've exhausted valid entries (all-zero == end) */
			if (g_sec[pos] == 0 && g_sec[pos+1] == 0 &&
			    g_sec[pos+2] == 0 && g_sec[pos+3] == 0) {
				break;
			}

			uint32_t entry_ino   = rd32(g_sec, pos);
			uint8_t  entry_nlen  = g_sec[pos + 4];
			if (entry_nlen == 0 || pos + 5 + entry_nlen > 512) {
				break;
			}

			if (entry_nlen == name_len &&
			    memcmp(g_sec + pos + 5, name, name_len) == 0) {
				return (int)entry_ino;
			}

			pos += 5 + entry_nlen;
		}
		off += 512 - (off % 512); /* advance to next sector boundary */
	}

	return -1;
}

int lafs_open(const char *path)
{
	if (!g_mounted) {
		return -1;
	}

	if (!path || path[0] != '/') {
		return -1;
	}

	uint32_t ino = 0; /* root */

	/* Skip leading '/' */
	const char *p = path + 1;
	while (*p) {
		/* Find next component */
		const char *start = p;
		while (*p && *p != '/') {
			p++;
		}
		uint32_t len = (uint32_t)(p - start);
		if (len == 0) {
			/* trailing slash or "//" — skip */
			if (*p) {
				p++;
			}
			continue;
		}

		int next = dir_lookup(ino, start, len);
		if (next < 0) {
			return -1;
		}
		ino = (uint32_t)next;

		if (*p == '/') {
			p++;
		}
	}

	return (int)ino;
}

/* File read */

int lafs_read(int ino, void *buf, uint32_t offset, uint32_t len)
{
	if (!g_mounted) {
		return -1;
	}
	if (len == 0) {
		return 0;
	}

	const uint8_t *raw = read_inode_raw(ino);
	if (!raw || raw[0] != 1)  /* must be a regular file */
		return -1;

	uint32_t fsize = rd32(raw, 33);
	if (offset >= fsize) {
		return 0;
	}
	if (offset + len > fsize) {
		len = fsize - offset;
	}

	uint32_t total = 0;
	uint8_t *dst = (uint8_t*)buf;

	while (total < len) {
		uint32_t blk_idx = offset / 512;
		uint32_t blk_off = offset % 512;
		if (blk_idx >= LAFS_BLOCKS_MAX) {
			break;
		}

		uint32_t blk_sec = rd32(raw, 37 + blk_idx * 4);
		if (blk_sec == 0) {
			break;
		}

		if (read_sector(blk_sec) != 0) {
			return total > 0 ? (int)total : -1;
		}

		uint32_t chunk = 512 - blk_off;
		if (chunk > len - total) {
			chunk = len - total;
		}

		memcpy(dst + total, g_sec + blk_off, chunk);
		total += chunk;
		offset += chunk;
	}

	return (int)total;
}

/* Directory listing */

int lafs_readdir(int ino, char *buf, uint32_t bufsz)
{
	if (!g_mounted || bufsz == 0) {
		return -1;
	}

	const uint8_t *raw = read_inode_raw(ino);
	if (!raw || raw[0] != 2) {
		return -1;
	}

	uint32_t dir_size = rd32(raw, 33);
	uint32_t written = 0;

	/* We need to scan the dir's data blocks while also reading target
	 * inodes (which reuse g_sec). Save each dir block locally. */
	static uint8_t dir_sec[512] __attribute__((aligned(16)));

	for (uint32_t off = 0; off < dir_size && written + 1 < bufsz; ) {
		uint32_t blk_idx = off / 512;
		if (blk_idx >= LAFS_BLOCKS_MAX) {
			break;
		}
		uint32_t blk_sec = rd32(raw, 37 + blk_idx * 4);
		if (blk_sec == 0) {
			break;
		}

		if (read_sector(blk_sec) != 0) {
			return written > 0 ? (int)written : -1;
		}
		memcpy(dir_sec, g_sec, 512);

		uint32_t pos = off % 512;
		while (pos + 5 <= 512 && written + 1 < bufsz) {
			if (dir_sec[pos] == 0 && dir_sec[pos+1] == 0 &&
			    dir_sec[pos+2] == 0 && dir_sec[pos+3] == 0) {
				goto done_block;
			}

			uint8_t entry_nlen = dir_sec[pos + 4];
			if (entry_nlen == 0 || pos + 5 + entry_nlen > 512) {
				break;
			}

			/* Get the entry's inode to read its name.
			 * read_inode_raw trashes g_sec — dir_sec is safe. */
			uint32_t entry_ino = rd32(dir_sec, pos);
			const uint8_t *entry_raw = read_inode_raw(entry_ino);
			if (entry_raw && entry_raw[0] != 0) {
				uint32_t name_len = 0;
				while (name_len < 32 && entry_raw[1 + name_len]) {
					name_len++;
				}
				if (name_len > 0) {
					uint32_t copy = name_len;
					if (written + copy + 1 >= bufsz) {
						copy = bufsz - written - 1;
					}
					memcpy(buf + written, entry_raw + 1, copy);
					written += copy;
					buf[written++] = '\n';
				}
			}
			pos += 5 + entry_nlen;
		}
done_block:
		off += 512 - (off % 512);
	}
	buf[written] = '\0';

	return (int)written;
}

/* File size */

uint32_t lafs_size(int ino)
{
	const uint8_t *raw = read_inode_raw(ino);
	if (!raw || raw[0] != 1) {
		return 0;
	}
	return rd32(raw, 33);
}
