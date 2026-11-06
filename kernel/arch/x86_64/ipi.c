/*
 * x86_64 核间中断发送与回调分发。
 *
 * 提供定向发送、广播、重调度通知，以及 TLB 和自测回调接口。
 */
#include "ipi.h"
#include "lapic.h"
#include "debug.h"
#include "hhdm.h"
#include "export.h"
#include "cpu.h"

#define IPI_MAX_CALLBACKS 4

// LAPIC 寄存器偏移 (假设已经映射了 LAPIC MMIO)
#define LAPIC_ICR_LOW  0x300
#define LAPIC_ICR_HIGH 0x310

#define LAPIC_BASE ((uint64_t)phys_to_virt(LAPIC_BASE_PHYS))

/* 由自测模块注册、在收到对应 IPI 的处理器上调用的 TLB 回调。 */
static ipi_callback_t g_tlb_callbacks[IPI_MAX_CALLBACKS];

/* 自测 IPI 回调，与 TLB 回调分开维护。 */
static ipi_callback_t g_selftest_callbacks[IPI_MAX_CALLBACKS];
static volatile uint64_t g_reschedule_count[MAX_CPUS];

static void ipi_callback_set(ipi_callback_t *table, ipi_callback_t cb)
{
	if (!cb) {
		return;
	}
	for (int i = 0; i < IPI_MAX_CALLBACKS; i++) {
		if (__atomic_load_n(&table[i], __ATOMIC_ACQUIRE) == cb) {
			return;
		}
	}
	for (int i = 0; i < IPI_MAX_CALLBACKS; i++) {
		if (!__atomic_load_n(&table[i], __ATOMIC_ACQUIRE)) {
			__atomic_store_n(&table[i], cb, __ATOMIC_RELEASE);
			return;
		}
	}
	kprintf("[ipi] callback table full\n");
}

static void ipi_callback_clear(ipi_callback_t *table, ipi_callback_t cb)
{
	if (!cb) {
		return;
	}
	for (int i = 0; i < IPI_MAX_CALLBACKS; i++) {
		if (__atomic_load_n(&table[i], __ATOMIC_ACQUIRE) == cb) {
			__atomic_store_n(&table[i], NULL, __ATOMIC_RELEASE);
			return;
		}
	}
}

static void ipi_callback_invoke(ipi_callback_t *table)
{
	for (int i = 0; i < IPI_MAX_CALLBACKS; i++) {
		ipi_callback_t cb = __atomic_load_n(&table[i], __ATOMIC_ACQUIRE);
		if (cb) {
			cb();
		}
	}
}

void ipi_tlb_set_callback(ipi_callback_t cb)
{
	ipi_callback_set(g_tlb_callbacks, cb);
}
EXPORT_SYMBOL(ipi_tlb_set_callback);

void ipi_tlb_clear_callback(ipi_callback_t cb)
{
	ipi_callback_clear(g_tlb_callbacks, cb);
}
EXPORT_SYMBOL(ipi_tlb_clear_callback);

void ipi_tlb_invoke_callbacks(void)
{
	ipi_callback_invoke(g_tlb_callbacks);
}

void ipi_selftest_set_callback(ipi_callback_t cb)
{
	ipi_callback_set(g_selftest_callbacks, cb);
}
EXPORT_SYMBOL(ipi_selftest_set_callback);

void ipi_selftest_clear_callback(ipi_callback_t cb)
{
	ipi_callback_clear(g_selftest_callbacks, cb);
}
EXPORT_SYMBOL(ipi_selftest_clear_callback);

void ipi_selftest_invoke_callbacks(void)
{
	ipi_callback_invoke(g_selftest_callbacks);
}

/* IPI 处理在被中断线程的内核栈上执行；当前入口未为普通 IPI 配置 IST。
 * 活动页表必须保持该内核栈及入口代码可访问。EOI 由 IDT 公共分发路径处理。 */
void ipi_send(uint8_t apic_id, uint8_t vector)
{
	/* 等待前一条 IPI 完成投递。 */
	while (*(volatile uint32_t *)(LAPIC_BASE + LAPIC_ICR_LOW) & (1 << 12)) {
		/* 等待本地 APIC 接受此前的 IPI。 */
	}

	/* 写入目标 APIC ID。 */
	*(volatile uint32_t *)(LAPIC_BASE + LAPIC_ICR_HIGH) = (uint32_t)apic_id << 24;

	/* 写入向量和固定投递模式以发送 IPI。 */
	*(volatile uint32_t *)(LAPIC_BASE + LAPIC_ICR_LOW) = (uint32_t)vector | 0x4000;
}

/* 向除当前处理器外的所有处理器广播 IPI。 */
void ipi_broadcast(uint8_t vector)
{
	L();
	while (*(volatile uint32_t *)(LAPIC_BASE + LAPIC_ICR_LOW) & (1 << 12)) {
		/* 等待本地 APIC 接受此前的 IPI。 */
	}
	L();
	/* Destination Shorthand 0xC0000 表示除当前处理器外的全部处理器。 */
	*(volatile uint32_t *)(LAPIC_BASE + LAPIC_ICR_LOW) = 0xC0000 | (uint32_t)vector | 0x4000;

	L();
}
EXPORT_SYMBOL(ipi_broadcast);

/*
 * 逻辑 CPU ID 是连续的调度器索引，xAPIC ID 则可能稀疏。在 per-CPU 上下文
 * 保存硬件 APIC ID 前，向其他 CPU 广播唤醒 IPI；只有 need_resched 已置位的
 * CPU 会执行重调度。之后可在建立 ID 映射时改为定向发送。
 */
void ipi_reschedule_cpu(uint32_t cpu_id)
{
	struct cpu_context *ctx;

	if (cpu_id >= MAX_CPUS) {
		return;
	}
	ctx = g_cpu_contexts[cpu_id];
	if (!ctx) {
		return;
	}

	__atomic_store_n(&ctx->need_resched, 1, __ATOMIC_RELEASE);
	if (ctx == cpu_get_ctx()) {
		return;
	}
	ipi_broadcast(IPI_VECTOR_RESCHEDULE);
}

void ipi_reschedule_ack(void)
{
	struct cpu_context *ctx = cpu_get_ctx();

	if (ctx && ctx->id >= 0 && ctx->id < MAX_CPUS) {
		__atomic_add_fetch(&g_reschedule_count[ctx->id], 1, __ATOMIC_RELEASE);
	}
}

uint64_t ipi_reschedule_count(uint32_t cpu_id)
{
	if (cpu_id >= MAX_CPUS) {
		return 0;
	}
	return __atomic_load_n(&g_reschedule_count[cpu_id], __ATOMIC_ACQUIRE);
}

EXPORT_SYMBOL(ipi_reschedule_count);
