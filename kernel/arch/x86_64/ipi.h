#ifndef __IPI_H__
#define __IPI_H__

/*
 * x86_64 IPI 向量号与发送、回调接口。
 */

#include <stdint.h>

/* 当前分配给内核 IPI 的向量号。 */
#define IPI_VECTOR_HALT	      0xFE /* 停止处理器；当前尚无处理入口。 */
#define IPI_VECTOR_TLB	      0xFD /* TLB shootdown。 */
#define IPI_VECTOR_SELFTEST   0xFC /* 自测回调。 */
#define IPI_VECTOR_RESCHEDULE 0xFB /* 远端运行队列唤醒。 */

void ipi_send(uint8_t apic_id, uint8_t vector);
void ipi_broadcast(uint8_t vector);
void ipi_reschedule_cpu(uint32_t cpu_id);
void ipi_reschedule_ack(void);
uint64_t ipi_reschedule_count(uint32_t cpu_id);

/* TLB IPI 完成刷新后调用的观测回调。 */
typedef void (*ipi_callback_t)(void);
void ipi_tlb_set_callback(ipi_callback_t cb);
void ipi_tlb_clear_callback(ipi_callback_t cb);
void ipi_tlb_invoke_callbacks(void);

/* 自测专用 IPI 回调链，与 TLB 回调分开维护。 */
void ipi_selftest_set_callback(ipi_callback_t cb);
void ipi_selftest_clear_callback(ipi_callback_t cb);
void ipi_selftest_invoke_callbacks(void);

#endif
