/*
 * lafs.h — LaFS 只读文件系统接口
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

#define LAFS_MAGIC	0x5346614c /* "LaFS", little-endian */
#define LAFS_INODE_SIZE 128
#define LAFS_NAME_MAX	31
#define LAFS_BLOCKS_MAX 20

/* 从第 0 扇区读取并验证超级块。成功返回 0，格式无效时返回 -1。 */
int lafs_mount(void);

/* 清除挂载元数据；不重置或注销块设备。 */
void lafs_reset(void);

/* 按绝对路径打开文件。成功返回非负 inode 索引，未找到时返回 -1。 */
int lafs_open(const char *path);

/* 从文件 inode 读取数据。返回读取字节数；到达文件末尾时返回 0，出错时返回负值。 */
int lafs_read(int ino, void *buf, uint32_t offset, uint32_t len);

/* 将目录项以换行分隔的名称写入 buf，最多写入 bufsz 字节（含终止符）。
 * 返回写入字节数；目录为空时返回 0，出错时返回负值。 */
int lafs_readdir(int ino, char *buf, uint32_t bufsz);

/* 返回 inode 对应文件的大小；目录返回 0。 */
uint32_t lafs_size(int ino);

#endif /* __LAFS_H__ */
