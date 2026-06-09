/*
 * kernel/debug.h - 诊断接口转发头
 *
 * 历史背景:debug.h 提供 L() / panic() / hcf() / interrupts_enabled() 等
 * 通用诊断宏，接口本身 arch-neutral，但当前 panic 实现内嵌 x86 asm，
 * 因此此文件转发到各架构的实现。
 *
 * 多 arch 支持：根据编译器预定义宏选择对应架构的 debug 实现。
 * 新增架构：复制一个 #elif 分支即可。
 */
#ifndef __KERNEL_DEBUG_FWD_H__
#define __KERNEL_DEBUG_FWD_H__

#if defined(__x86_64__)
#  include "arch/x86_64/debug.h"
#elif defined(__aarch64__)
#  include "arch/aarch64/debug.h"
#else
#  error "unsupported architecture for debug.h"
#endif

#endif
