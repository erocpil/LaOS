/*
 * lafs.h — LaFS minimal read-only filesystem
 *
 * LaFS superblock (sector 0):
 *   offset 0: magic "LaFS\x01" (5 bytes)
 *   offset 5: inode_count   (uint32)
 *   offset 9: inode_start   (uint32, sector of inode table)
 *   offset 13: data_start   (uint32, sector of first data block)
 *   offset 17: root_inode   (uint32, always 0)
 *
 * Inode (128 bytes):
 *   offset 0:  type         (uint8, 0=free 1=file 2=dir)
 *   offset 1:  name         (char[32], null-padded)
 *   offset 33: size         (uint32, file size)
 *   offset 37: blocks[20]   (uint32, direct block pointers)
 */

#ifndef __LAFS_H__
#define __LAFS_H__

#include <stdint.h>

#define LAFS_MAGIC 0x5346614c /* "LaFS", little-endian */
#define LAFS_INODE_SIZE 128
#define LAFS_NAME_MAX 31
#define LAFS_BLOCKS_MAX 20

/* Initialize the filesystem by reading superblock from sector 0.
 * Returns 0 on success, -1 on bad magic. */
int lafs_mount(void);

/* Open a file by absolute path (e.g. "/etc/motd").
 * Returns inode index (>=0) on success, -1 on not found. */
int lafs_open(const char *path);

/* Read from a file inode. Returns bytes read (<= len), 0 at EOF, <0 on error. */
int lafs_read(int ino, void *buf, uint32_t offset, uint32_t len);

/* Read directory entries as newline-separated names.
 * Writes at most bufsz bytes (including null terminator) to buf.
 * Returns total bytes written, 0 if empty, <0 on error. */
int lafs_readdir(int ino, char *buf, uint32_t bufsz);

/* Get file size for an inode. Returns 0 for directories. */
uint32_t lafs_size(int ino);

#endif /* __LAFS_H__ */
