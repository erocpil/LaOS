#ifndef __HHDM_H__
#define __HHDM_H__

/*
 * hhdm.h - Higher Half Direct Map 基地址
 */

#include <stdint.h>

#include "export.h"

/* HHDM (Higher Half Direct Map): Limine 把全部物理内存线性映射到内核虚拟地址空间
 * 的高半区。本模块封装 offset 的初始化，查询，以及最常用的物理/虚拟地址转换。
 *
 * 设计取舍：用函数而非裸全局变量
 *   - 内核启动早期 hhdm_offset 为 0，访问要 assert 防止误用
 *   - 模块通过 ksym 拿函数地址实现 PC32 重定位，比直接拿变量地址少一次
 *     R_X86_64_64 + 间接访存
 *   - 实现需要时可以改成 percpu / 动态 remap，调用方零变更
 */

/** 启动期一次性写入；多次调用同值无副作用，传不同值触发 panic。 */
void hhdm_init(uint64_t offset);

/* 返回 HHDM 偏移；未初始化时触发 panic。 */
uint64_t hhdm_offset(void);

/** phys/virt 转换。phys 必须在 limine 报告的 usable 区段内。 */
static inline void *phys_to_virt(uint64_t phys)
{
	return (void*)(phys + hhdm_offset());
}

static inline uint64_t virt_to_phys(void *virt)
{
	return (uint64_t)virt - hhdm_offset();
}

EXPORT_SYMBOL(hhdm_offset);

#endif
