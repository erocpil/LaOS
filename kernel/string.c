/*
 * string.c — 字符串与内存操作
 *
 * memcpy/memset/memcmp/strcmp/strcpy/strlen 等库函数。
 */
#include <stdint.h>

#include "string.h"
#include "export.h"

// 这些符号由 freestanding 内核和 Limine 模板依赖；调整实现位置时须保留名称和 ABI。

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
	uint8_t *p = (uint8_t *)s;

	for (size_t i = 0; i < n; i++) {
		p[i] = (uint8_t)c;
	}

	return s;
}
EXPORT_SYMBOL(memset);

void *memmove(void *dest, const void *src, size_t n)
{
	uint8_t *pdest = (uint8_t *)dest;
	const uint8_t *psrc = (const uint8_t *)src;

	if (src > dest) {
		for (size_t i = 0; i < n; i++) {
			pdest[i] = psrc[i];
		}
	} else if (src < dest) {
		for (size_t i = n; i > 0; i--) {
			pdest[i - 1] = psrc[i - 1];
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

/* 查找 needle 在 haystack 中首次出现的位置。
 *
 * 未找到时返回 NULL；needle 为空时返回 haystack。
 */
char *strstr(const char *haystack, const char *needle)
{
	// 空 needle 按字符串搜索约定匹配于 haystack 起始处。
	if (*needle == '\0') {
		return (char *)haystack;
	}

	// 逐个检查可能的匹配起点。
	for (; *haystack != '\0'; haystack++) {
		// 首字符不匹配时无需比较剩余字符。
		if (*haystack != *needle) {
			continue;
		}

		// 比较从当前起点开始的完整 needle。
		const char *h = haystack;
		const char *n = needle;

		// 继续比较，直至 needle 匹配完成或出现差异。
		while (*h != '\0' && *n != '\0' && *h == *n) {
			h++;
			n++;
		}

		// 到达 needle 末尾表示匹配成功。
		if (*n == '\0') {
			return (char *)haystack;
		}
	}

	// 所有候选起点均未匹配。
	return NULL;
}
EXPORT_SYMBOL(strstr);
