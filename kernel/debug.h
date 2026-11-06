/*
 * kernel/debug.h - 诊断接口转发头
 *
 * 本文件汇总 L()、panic()、hcf()、interrupts_enabled() 等
 * 通用诊断接口。接口与架构无关，但 panic 的实现包含架构相关汇编，
 * 因此此文件转发到各架构的实现。
 *
 * 多 arch 支持：根据编译器预定义宏选择对应架构的 debug 实现。
 * 新增架构时，应在下方增加对应的条件分支和实现头文件。
 */
#ifndef __KERNEL_DEBUG_FWD_H__
#define __KERNEL_DEBUG_FWD_H__

#if defined(__x86_64__)
#include "arch/x86_64/debug.h"
#elif defined(__aarch64__)
#include "arch/aarch64/debug.h"
#else
#error "unsupported architecture for debug.h"
#endif

#endif
