#ifndef __STRING_H__
#define __STRING_H__

/*
 * string.h - 字符串与内存操作函数
 */

#include <stddef.h> // 为了使用 size_t 和 NULL
#include <stdbool.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
char *strstr(const char *haystack, const char *needle);
int strcmp(const char *s1, const char *s2);

#endif
