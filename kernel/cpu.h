/*
 * kernel/cpu.h — per-CPU 上下文接口转发头
 *
 * 根据目标架构包含对应的 arch/<arch>/cpu.h。通用内核代码和模块通过此头文件
 * 使用 per-CPU 接口，无需直接依赖架构私有定义。
 */
#ifndef __KERNEL_CPU_FWD_H__
#define __KERNEL_CPU_FWD_H__

#if defined(__x86_64__)
#include "arch/x86_64/cpu.h"
#elif defined(__aarch64__)
#include "arch/aarch64/cpu.h"
#else
#error "unsupported architecture"
#endif

/* 跨架构的调度队列接口。声明集中在转发头中，避免通用代码和内置模块
 * 直接包含架构私有的 cpu.h。 */
void cpu_enqueue(int cpu_id, struct thread *t);
void cpu_enqueue_tail(int cpu_id, struct thread *t);
struct thread *cpu_dequeue(int cpu_id);
void cpu_enqueue_zombie(int cpu_id, struct thread *t);
struct thread *cpu_dequeue_zombie(int cpu_id);
struct thread *cpu_dequeue_zombie_tail(int cpu_id);

#endif
