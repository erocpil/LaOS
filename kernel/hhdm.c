/*
 * hhdm.c — 高半区直接映射（HHDM）初始化
 *
 * x86_64 由 Limine 提供 HHDM 偏移量；AArch64 当前使用恒等映射，偏移量为 0。
 */
#include "hhdm.h"

#include <stdbool.h>
#include "debug.h"

static uint64_t s_hhdm_offset;
static bool s_hhdm_initialized;

void hhdm_init(uint64_t offset)
{
	if (s_hhdm_initialized && s_hhdm_offset != offset) {
		panic("hhdm_init: re-init with different offset old=0x%lx new=0x%lx", s_hhdm_offset,
			offset);
	}
	s_hhdm_offset = offset;
	s_hhdm_initialized = true;
}

uint64_t hhdm_offset(void)
{
	if (!s_hhdm_initialized) {
		panic("hhdm_offset: not initialized");
	}
	return s_hhdm_offset;
}
