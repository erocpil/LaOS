#ifndef __USER_LIB_H__
#define __USER_LIB_H__

#include <stdint.h>

void exit(int status);
int write(int fd, const void *buf, unsigned long count);
int msleep(unsigned int msec);

/* mmap/munmap 系统调用封装。 */
void *mmap(uint64_t length, uint64_t prot, uint64_t flags);
int munmap(uint64_t addr, uint64_t length);

/* 保护和映射标志须与 kernel/vma.h 保持一致。 */
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_LAZY   0x100 /* 仅建立 VMA，首次访问时再分配物理页。 */

#endif
