/*
 * cpio.h — SVR4 newc CPIO archive parser
 *
 * Minimal parser for initrd CPIO archives. Supports only newc format
 * ("070701" magic). Provides sequential iteration and name-based lookup.
 */

#ifndef __CPIO_H__
#define __CPIO_H__

#include <stdint.h>

/* A single file entry within a CPIO archive */
struct cpio_entry {
	const char *name; /* filename (null-terminated) */
	const void *data; /* file contents */
	uint32_t size; /* file size in bytes */
};

/*
 * Initialise the CPIO parser with an in-memory archive.
 * Returns 0 on success, -1 if the archive is not valid newc format.
 */
int cpio_init(const void *archive, uint32_t size);

/*
 * Iterate over all files in the archive.
 * Returns 1 and fills *entry if a file was found.
 * Returns 0 when the end of the archive (TRAILER!!!) is reached.
 * Returns -1 on parse error.
 *
 * The caller should NOT free *entry; the data pointer points into
 * the archive buffer passed to cpio_init.
 */
int cpio_next(struct cpio_entry *entry);

/*
 * Find a file by exact name match. Returns 1 + fills *entry on
 * success, 0 if not found, -1 on error.
 */
int cpio_find(const char *name, struct cpio_entry *entry);

#endif /* __CPIO_H__ */
