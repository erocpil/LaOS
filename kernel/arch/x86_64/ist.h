/*
 * x86_64 IST（Interrupt Stack Table）栈管理接口。
 *
 * x86_64 TSS 提供 7 个 IST slot(tss.ist[0..6]).当 IDT entry 的 ist 字段
 * 非零时，硬件在进入中断时**强制切换到 IST 栈**(不使用当前线程的 rsp).
 *
 * 用途：保护栈溢出场景。若被抢占线程的 kernel stack 已接近耗尽，此时进入
 * double-fault / NMI 等致命异常若继续用同一个栈，会触发二次栈溢出 ->
 * 三重故障。IST 为这些入口提供独立栈，可降低继续使用损坏或耗尽栈的风险，
 * 但不保证异常处理过程本身不会再次故障。
 *
 * LaOS 分配策略：
 *   slot 0: 保留不用(Intel SDM 约定:ist=0 表示"不切栈")
 *   slot 1 (IST_SLOT_DF):  double-fault (#DF, vector 8)
 *   slot 2 (IST_SLOT_NMI): NMI (vector 2)
 *   slot 3 (IST_SLOT_MCE): machine check (#MC, vector 18)
 *   slot 4–6: TSS 中未分配的槽位；当前没有对应的 IST 栈。
 *
 * 每个已用栈为 8 KiB，由静态 BSS 数组提供，启动早期即可访问。
 */
#ifndef __IST_H__
#define __IST_H__

#include <stdint.h>

#define NR_IST_SLOTS	4 // 当前只用 slot 1/2/3；预留一位给未来 slot 4
#define IST_STACK_PAGES 2
#define IST_STACK_SIZE	(IST_STACK_PAGES * 4096)

#define IST_SLOT_DF  1
#define IST_SLOT_NMI 2
#define IST_SLOT_MCE 3

/*
 * 为指定 CPU 准备 IST 栈，并将栈顶写入其 TSS。
 * cpu_id 是内核使用的逻辑 CPU 编号。
 *
 * 需在 gdt_init_cpu 完成 TSS memset 后调用。
 * 幂等：多次调用只覆盖 tss.ist[] 字段，不重新分配栈。
 */
void ist_init_cpu(int cpu_id);

/*
 * ist_stack_top - 取指定 CPU / slot 的 IST 栈顶(对齐后可写入 tss.ist[])
 */
uint64_t ist_stack_top(int cpu_id, int slot);

#endif
