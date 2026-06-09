/*
 * cpio.c — SVR4 newc CPIO archive parser
 *
 * Parses in-memory newc ("070701") CPIO archives. Used to load
 * user ELFs and task configs from an initrd at boot time.
 *
 * newc header layout (110 ASCII bytes):
 *   6B magic ("070701"), then 13 × 8 hex fields:
 *   ino, mode, uid, gid, nlink, mtime, filesize,
 *   devmajor, devminor, rdevmajor, rdevminor, namesize, check
 *
 * After the header: filename (null-terminated, 4-byte aligned),
 * then file data (4-byte aligned).
 */

#include "cpio.h"
#include "string.h"

#define CPIO_MAGIC "070701"
#define CPIO_MAGIC_LEN 6
#define CPIO_HDR_LEN 110 /* total header size */
#define CPIO_NAME_TERM "TRAILER!!!"

/* Internal state — reinitialised on each cpio_init() call */
static const uint8_t *g_archive;
static uint32_t g_archive_size;
static uint32_t g_offset; /* current parse cursor */

/* Parse an 8-char hex field from the header */
static uint32_t parse_hex32(const char *s)
{
	uint32_t v = 0;
	for (int i = 0; i < 8; i++) {
		char c = s[i];
		v <<= 4;
		if (c >= '0' && c <= '9') {
			v |= (c - '0');
		} else if (c >= 'A' && c <= 'F') {
			v |= (c - 'A' + 10);
		} else if (c >= 'a' && c <= 'f') {
			v |= (c - 'a' + 10);
		}
	}
	return v;
}

int cpio_init(const void *archive, uint32_t size)
{
	if (!archive || size < CPIO_HDR_LEN + 1) {
		return -1;
	}

	if (memcmp(archive, CPIO_MAGIC, CPIO_MAGIC_LEN) != 0) {
		return -1;
	}

	g_archive = (const uint8_t*)archive;
	g_archive_size = size;
	g_offset = 0;

	return 0;
}

/* Round up to next multiple of 4 */
static inline uint32_t pad4(uint32_t n)
{
	return (n + 3) & ~3U;
}

int cpio_next(struct cpio_entry *entry)
{
	if (!g_archive || !entry) {
		return -1;
	}

	while (g_offset + CPIO_HDR_LEN <= g_archive_size) {
		const char *hdr = (const char*)&g_archive[g_offset];

		/* Check magic */
		if (memcmp(hdr, CPIO_MAGIC, CPIO_MAGIC_LEN) != 0) {
			return -1;
		}

		uint32_t namesize = parse_hex32(hdr + 94); /* c_namesize at offset 94 */
		uint32_t filesize = parse_hex32(hdr + 54); /* c_filesize at offset 54 */

		if (namesize == 0) {
			return -1; /* zero-length name is invalid */
		}

		uint32_t name_start = g_offset + CPIO_HDR_LEN;
		if (name_start + namesize > g_archive_size) {
			return -1;
		}

		const char *name = (const char*)&g_archive[name_start];

		/* Check for trailer */
		if (memcmp(name, CPIO_NAME_TERM, 10) == 0) {
			return 0; /* end of archive */
		}

		/* Advance past header + padded name */
		uint32_t data_start = name_start + pad4(namesize);
		if (data_start + filesize > g_archive_size) {
			return -1;
		}

		entry->name = name;
		entry->data = &g_archive[data_start];
		entry->size = filesize;

		/* Advance cursor to next header */
		g_offset = data_start + pad4(filesize);

		return 1;
	}

	return 0; /* end of buffer reached */
}

int cpio_find(const char *name, struct cpio_entry *entry)
{
	if (!name || !entry) {
		return -1;
	}

	/* Re-initialise cursor to start of archive */
	g_offset = 0;

	struct cpio_entry e;
	int ret;
	while ((ret = cpio_next(&e)) == 1) {
		if (strcmp(e.name, name) == 0) {
			*entry = e;
			return 1;
		}
	}

	return ret; /* 0 = not found, -1 = error */
}
