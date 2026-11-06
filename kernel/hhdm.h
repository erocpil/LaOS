#ifndef __HHDM_H__
#define __HHDM_H__

/*
 * hhdm.h — 高半区直接映射（HHDM）接口
 */

#include <stdint.h>

#include "export.h"

/* HHDM（Higher Half Direct Map）将物理地址映射到内核虚拟地址空间。本模块
 * 提供偏移量的初始化与查询，以及物理地址和虚拟地址之间的转换。
 *
 * 偏移量通过函数接口访问，使调用方不依赖其存储形式；启动早期若偏移量
 * 尚未初始化，查询接口会触发 panic。
 */

/* 设置 HHDM 偏移量。重复设置相同值无副作用；设置不同值会触发 panic。 */
void hhdm_init(uint64_t offset);

/* 返回 HHDM 偏移；未初始化时触发 panic。 */
uint64_t hhdm_offset(void);

/* 在物理地址与 HHDM 虚拟地址之间转换。调用方须保证地址位于映射范围内。 */
static inline void *phys_to_virt(uint64_t phys)
{
	return (void *)(phys + hhdm_offset());
}

static inline uint64_t virt_to_phys(void *virt)
{
	return (uint64_t)virt - hhdm_offset();
}

EXPORT_SYMBOL(hhdm_offset);

#endif
