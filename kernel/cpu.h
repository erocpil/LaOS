/*
 * kernel/cpu.h - per-CPU context 接口转发头
 *
 * 按编译器架构宏分发到正确的 arch/<arch>/cpu.h。
 * module 等子目录始终 #include "../kernel/cpu.h"，无需感知 ARCH。
 */
#ifndef __KERNEL_CPU_FWD_H__
#define __KERNEL_CPU_FWD_H__

#if defined(__x86_64__)
#  include "arch/x86_64/cpu.h"
#elif defined(__aarch64__)
#  include "arch/aarch64/cpu.h"
#else
#  error "unsupported architecture"
#endif

/* Cross-architecture scheduler queue API.  Keep these declarations in the
 * forwarding header so common kernel code and built-in modules do not need
 * to include an architecture-private cpu.h directly. */
void cpu_enqueue(int cpu_id, struct thread *t);
void cpu_enqueue_tail(int cpu_id, struct thread *t);
struct thread *cpu_dequeue(int cpu_id);
void cpu_enqueue_zombie(int cpu_id, struct thread *t);
struct thread *cpu_dequeue_zombie(int cpu_id);
struct thread *cpu_dequeue_zombie_tail(int cpu_id);

#endif
