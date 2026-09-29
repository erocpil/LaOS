/*
 * arch_dispatch.h — 架构分发层
 *
 * 原则：本文件是唯一使用 #ifdef 选择架构的地方。
 * 所有 kernel/ 代码通过本文件引入的接口使用架构功能，
 * 不做散落的 #ifdef __x86_64__。
 *
 * 新增架构：在本文件加一个 #elif 分支 + 对应的 arch/<arch>/ 目录即可。
 *
 * 当前状态：
 *   x86_64  — 完整实现
 *   aarch64 — stub 就绪，待填充实现
 *   riscv64 — stub 就绪，待填充实现
 */

#ifndef __ARCH_DISPATCH_H__
#define __ARCH_DISPATCH_H__

/* 第一组：CPU / IRQ / barrier / TLB */
#if defined(__x86_64__)
#  define KERNEL_ARCH_x86_64 1
#  include "arch/x86_64/arch_irq.h"
#  include "arch/x86_64/arch_barrier.h"
#  include "arch/x86_64/arch_cpu.h"
#  include "arch/x86_64/arch_tlb.h"
#elif defined(__aarch64__)
#  define KERNEL_ARCH_aarch64 1
#  include "arch/aarch64/arch_irq.h"
#  include "arch/aarch64/arch_barrier.h"
#  include "arch/aarch64/g_its.h"
#  include "arch/aarch64/arch_cpu.h"
#  include "arch/aarch64/arch_tlb.h"
#elif defined(__riscv) && (__riscv_xlen == 64)
#  define KERNEL_ARCH_riscv64 1
#  include "arch/riscv64/arch_irq.h"
#  include "arch/riscv64/arch_barrier.h"
#  include "arch/riscv64/arch_cpu.h"
#  include "arch/riscv64/arch_tlb.h"
#else
#  error "unsupported architecture"
#endif

/* 第二组：线程 / 入口 / 串口 */
#if defined(__x86_64__)
#  include "arch/x86_64/thread_arch.h"
#  include "arch/x86_64/entry_arch.h"
#  include "arch/x86_64/serial_arch.h"
#elif defined(__aarch64__)
#  include "arch/aarch64/thread_arch.h"
#  include "arch/aarch64/entry_arch.h"
#  include "arch/aarch64/serial_arch.h"
#elif defined(__riscv) && (__riscv_xlen == 64)
#  include "arch/riscv64/thread_arch.h"
#  include "arch/riscv64/entry_arch.h"
#  include "arch/riscv64/serial_arch.h"
#endif

/* 第三组：VMM 页表标志位 */
#if defined(__x86_64__)
#  include "arch/x86_64/vmm_arch.h"
#elif defined(__aarch64__)
#  include "arch/aarch64/vmm_arch.h"
#elif defined(__riscv) && (__riscv_xlen == 64)
#  include "arch/riscv64/vmm_arch.h"
#endif

/* 第四组：内存布局常量 */
#if defined(__x86_64__)
#  define KHEAP_VBASE 0xffffa00000000000
#  define MODULE_REGION_DEFAULT_BASE 0xffffffffc0000000ULL
#  define MODULE_REGION_SIZE         0x40000000ULL /* 1 GB */
#elif defined(__aarch64__)
#  define KHEAP_VBASE 0x40140000
#  define MODULE_REGION_SIZE         0x04000000ULL /* 64 MB */
#  define MODULE_REGION_OFFSET       0x02000000ULL /* kernel + 32 MB */
#elif defined(__riscv) && (__riscv_xlen == 64)
#  define KHEAP_VBASE 0xffffa00000000000
#endif

/* arch_module_alloc_base — 返回模块加载区的虚拟地址基址 */
uintptr_t arch_module_alloc_base(void);

#endif /* __ARCH_DISPATCH_H__ */
