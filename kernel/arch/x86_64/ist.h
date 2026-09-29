/*
 * ist.h - IST (Interrupt Stack Table) 管理
 *
 * x86_64 TSS 提供 7 个 IST slot(tss.ist[0..6]).当 IDT entry 的 ist 字段
 * 非零时，硬件在进入中断时**强制切换到 IST 栈**(不使用当前线程的 rsp).
 *
 * 用途：保护栈溢出场景。若被抢占线程的 kernel stack 已接近耗尽，此时进入
 * double-fault / NMI 等致命异常若继续用同一个栈，会触发二次栈溢出 ->
 * triple-fault -> CPU 重启.IST 切栈保证异常处理程序总有干净栈可用。
 *
 * LaOS 分配策略：
 *   slot 0: 保留不用(Intel SDM 约定:ist=0 表示"不切栈")
 *   slot 1 (IST_SLOT_DF):  double-fault (#DF, vector 8)
 *   slot 2 (IST_SLOT_NMI): NMI (vector 2)
 *   slot 3 (IST_SLOT_MCE): machine check (#MC, vector 18)
 *   slot 4-6: 预留未来使用(如时钟中断，e1000 IRQ)
 *
 * 栈大小 8KB (2 页)；静态 BSS 分配，boot 早期即可用。
 */
#ifndef __IST_H__
#define __IST_H__

#include <stdint.h>

#define NR_IST_SLOTS      4   // 当前只用 slot 1/2/3；预留一位给未来 slot 4
#define IST_STACK_PAGES   2
#define IST_STACK_SIZE    (IST_STACK_PAGES * 4096)

#define IST_SLOT_DF   1
#define IST_SLOT_NMI  2
#define IST_SLOT_MCE  3

/**
 * ist_init_cpu - 初始化指定 CPU 的 IST 栈并填入 TSS
 * @cpu_id: 目标 CPU 编号
 *
 * 需在 gdt_init_cpu 完成 TSS memset 后调用。
 * 幂等：多次调用只覆盖 tss.ist[] 字段，不重新分配栈。
 */
void ist_init_cpu(int cpu_id);

/**
 * ist_stack_top - 取指定 CPU / slot 的 IST 栈顶(对齐后可写入 tss.ist[])
 */
uint64_t ist_stack_top(int cpu_id, int slot);

#endif
