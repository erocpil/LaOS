#ifndef __USER_LIB_H__
#define __USER_LIB_H__

#include <stdint.h>

void exit(int status);
int write(int fd, const void *buf, unsigned long count);
int msleep(unsigned int msec);

/* mmap/munmap — syscall wrappers (currently aarch64 only) */
void *mmap(uint64_t length, uint64_t prot, uint64_t flags);
int munmap(uint64_t addr, uint64_t length);

/* mmap flags (must match kernel/vma.h) */
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_LAZY   0x100  /* VMA only — no physical allocation */

#endif
