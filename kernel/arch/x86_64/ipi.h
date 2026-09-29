#ifndef __IPI_H__
#define __IPI_H__

/*
 * ipi.h - IPI 向量号定义与函数声明
 */

#include <stdint.h>

// 在 x86 中，建议使用 0x40 - 0xFF 之间的向量号。
// 这里定义一个用于 TLB 刷新的 IPI.
#define IPI_VECTOR_HALT       0xFE  // 强制停机 IPI
#define IPI_VECTOR_TLB        0xFD  // TLB Shootdown IPI
#define IPI_VECTOR_SELFTEST   0xFC  // Selftest IPI
#define IPI_VECTOR_RESCHEDULE 0xFB  // Remote runqueue wakeup

void ipi_send(uint8_t apic_id, uint8_t vector);
void ipi_broadcast(uint8_t vector);
void ipi_reschedule_cpu(uint32_t cpu_id);
void ipi_reschedule_ack(void);
uint64_t ipi_reschedule_count(uint32_t cpu_id);

/* Callback for TLB shootdown observers (selftest, diagnostics).
 * Registered callback runs on every AP after arch_tlb_flush_all(). */
typedef void (*ipi_callback_t)(void);
void ipi_tlb_set_callback(ipi_callback_t cb);
void ipi_tlb_clear_callback(ipi_callback_t cb);
void ipi_tlb_invoke_callbacks(void);

/* Dedicated selftest IPI callback chain — isolated from TLB. */
void ipi_selftest_set_callback(ipi_callback_t cb);
void ipi_selftest_clear_callback(ipi_callback_t cb);
void ipi_selftest_invoke_callbacks(void);

#endif
