/*
 * string.c - 字符串与内存操作
 *
 * memcpy/memset/memcmp/strcmp/strcpy/strlen 等库函数。
 */
#include <stdint.h>

#include "string.h"
#include "export.h"

// 按 C 规范实现。
// 请勿删除或重命名这些函数：否则工具链会出问题(limine 模板依赖它们).
// 它们可以迁移到另一个 .c 文件。

size_t strlen(const char *s)
{
	size_t n = 0;

	if (!s) {
		return n;
	}
	while (*s != '\0') {
		n++;
		s++;
	}

	return n;
}
EXPORT_SYMBOL(strlen);

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }

    return (unsigned char)*s1 - (unsigned char)*s2;
}
EXPORT_SYMBOL(strcmp);

void *memcpy(void *restrict dest, const void *restrict src, size_t n)
{
	uint8_t *restrict pdest = (uint8_t *restrict)dest;
	const uint8_t *restrict psrc = (const uint8_t *restrict)src;

	for (size_t i = 0; i < n; i++) {
		pdest[i] = psrc[i];
	}

	return dest;
}
EXPORT_SYMBOL(memcpy);

void *memset(void *s, int c, size_t n)
{
	uint8_t *p = (uint8_t*)s;

	for (size_t i = 0; i < n; i++) {
		p[i] = (uint8_t)c;
	}

	return s;
}
EXPORT_SYMBOL(memset);

void *memmove(void *dest, const void *src, size_t n)
{
	uint8_t *pdest = (uint8_t*)dest;
	const uint8_t *psrc = (const uint8_t *)src;

	if (src > dest) {
		for (size_t i = 0; i < n; i++) {
			pdest[i] = psrc[i];
		}
	} else if (src < dest) {
		for (size_t i = n; i > 0; i--) {
			pdest[i-1] = psrc[i-1];
		}
	}

	return dest;
}
EXPORT_SYMBOL(memmove);

int memcmp(const void *s1, const void *s2, size_t n)
{
	const uint8_t *p1 = (const uint8_t *)s1;
	const uint8_t *p2 = (const uint8_t *)s2;

	for (size_t i = 0; i < n; i++) {
		if (p1[i] != p2[i]) {
			return p1[i] < p2[i] ? -1 : 1;
		}
	}

	return 0;
}
EXPORT_SYMBOL(memcmp);

/** strstr() - 查找子串在字符串中首次出现的位置
 *
 * haystack:被查找的字符串。
 * needle:要查找的子串。
 *
 * 返回值：指向匹配位置的指针；未找到返回 NULL；needle 为空返回 haystack.
 */
char *strstr(const char *haystack, const char *needle)
{
	// 1. 特殊情况处理：如果 needle 是空字符串，直接返回 haystack
	if (*needle == '\0') {
		return (char*)haystack;
	}

	// 2. 遍历 haystack 的每一个字符
	for (; *haystack != '\0'; haystack++) {
		// 如果当前字符与 needle 的第一个字符不匹配，直接跳过(优化性能)
		if (*haystack != *needle) {
			continue;
		}

		// 3. 尝试匹配整个 needle
		const char *h = haystack;
		const char *n = needle;

		// 同时向后移动，直到 needle 结束或出现不匹配
		while (*h != '\0' && *n != '\0' && *h == *n) {
			h++;
			n++;
		}

		// 4. 如果 n 指向了 '\0'，说明 needle 已经全部匹配完成
		if (*n == '\0') {
			return (char*)haystack;
		}
	}

	// 5. 遍历完整个 haystack 仍未找到
	return NULL;
}
EXPORT_SYMBOL(strstr);
