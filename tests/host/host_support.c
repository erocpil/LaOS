#include <stddef.h>
#include <stdlib.h>
#include "host_support.h"

int allocations;
int fail_after = -1;
void *kmalloc(size_t size)
{
	if (fail_after == 0) {
		return NULL;
	}
	if (fail_after > 0) {
		fail_after--;
	}
	void *p = malloc(size);
	if (p) {
		allocations++;
	}
	return p;
}
void kfree(void *p)
{
	if (p) {
		allocations--;
		free(p);
	}
}
void kprintf(const char *fmt, ...)
{
	(void)fmt;
}
