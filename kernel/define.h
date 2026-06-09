#ifndef __DEFINE_H__
#define __DEFINE_H__

/*
 * define.h - 内核通用常量与辅助宏
 */

#define MAX_CPUS 16
#define PAGE_SIZE 4096
#define STACK_SIZE (PAGE_SIZE << 5)

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#endif
