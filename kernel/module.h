/*
 * module.h - 内核模块类型定义与注册表/arch 接口
 *
 * 模块注册表是 append-only 固定容量数组，不提供 remove() 或 unload()。
 * 真正的卸载需要：free-list 分配器、引用计数、线程停止、selftest
 * 注销、符号依赖及跨 CPU 指令同步，不是替换 bump 分配器就能完成的。
 */

#ifndef __MODULE_H__
#define __MODULE_H__

#include <stddef.h>

/* 模块用途分类 */
enum module_kind {
	MODULE_KIND_KTHREAD,  /* task 线程加载的模块 */
	MODULE_KIND_SELFTEST, /* selftest 载荷 */
};

/* BSS 段描述符 — 模块可能有多段非连续 BSS */
#define MODULE_MAX_BSS_SEGMENTS 8

struct module_bss_segment {
	void  *base;
	size_t size;
};

/* 模块描述符 — 注册表条目，加载成功后填充 */
struct module_desc {
	const char                *name; /* 已复制到持久存储 */
	enum module_kind           kind;
	void                      *base; /* 代码+数据 VA 基址 */
	size_t                     size; /* 代码+数据大小（不含 BSS） */
	struct module_bss_segment  bss_segments[MODULE_MAX_BSS_SEGMENTS];
	int                        bss_count;
	void                      *entry; /* main / _start */
	void                      *init; /* selftest_init（kthread 为 NULL） */
	unsigned int               flags;
	unsigned int               id; /* 唯一注册表 ID */
};

/** 注册表 API（仅供加载成功后调用）*/

/* 预留一个注册表槽位。拷贝 name 到 kmalloc 持久存储。
 * 返回 id >= 0 供后续提交/取消；-1 表示注册表满或 kmalloc 失败。 */
int module_registry_reserve(struct module_desc *desc);

/* 提交预留槽位，使其对查询可见。 */
void module_registry_commit(unsigned int id);

/* 取消预留槽位，释放 name 副本。失败路径调用。 */
void module_registry_cancel(unsigned int id);

/* 查询 */
const struct module_desc *module_registry_find_by_id(unsigned int id);
size_t module_registry_count(void);
void module_registry_dump(void);

/** 架构接口 */

/*
 * 模块重定位修改了可执行代码后同步 I-cache。
 * x86_64: 硬件侦听保证一致性，仅编译器屏障。
 * AArch64: DC CVAU + DSB + IC IVAU + DSB + ISB，步长读 CTR_EL0。
 */
void arch_module_sync_icache(void *start, size_t size);

#endif /* __MODULE_H__ */
