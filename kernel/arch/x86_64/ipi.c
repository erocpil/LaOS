/*
 * ipi.c - 核间中断发送
 *
 * 当前仅提供 ipi_broadcast(向所有其他 CPU 广播 vector).
 * 用于 TLB 刷新和核间同步。
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

/* TLB IPI callbacks — registered by selftest modules, invoked on APs. */
static ipi_callback_t g_tlb_callbacks[IPI_MAX_CALLBACKS];

/* Selftest IPI callbacks — isolated from TLB chain. */
static ipi_callback_t g_selftest_callbacks[IPI_MAX_CALLBACKS];
static volatile uint64_t g_reschedule_count[MAX_CPUS];

static void ipi_callback_set(ipi_callback_t *table, ipi_callback_t cb)
{
	if (!cb)
		return;
	for (int i = 0; i < IPI_MAX_CALLBACKS; i++) {
		if (__atomic_load_n(&table[i], __ATOMIC_ACQUIRE) == cb)
			return;
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
	if (!cb)
		return;
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
		ipi_callback_t cb =
			__atomic_load_n(&table[i], __ATOMIC_ACQUIRE);
		if (cb)
			cb();
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

/*
   关键注意事项 (Debug 经验)
   EOI 顺序：对于 IPI，务必在执行复杂的刷新逻辑前先调用 EOI().否则，
   如果处理过程中触发了其他中断，可能会因为优先级嵌套导致 LAPIC 锁死。

   死锁风险：如果在发送 IPI 时持有了某个自旋锁(Spinlock)，而接收方正在尝试获取同一个锁，
   就会发生死锁(发送方等接收方处理 IPI，接收方等发送方释放锁).

   Spurious Interrupts:代码里处理了 0xFF 的伪中断，这非常好。
   在多核 IPI 压力大时，这种伪中断发生的概率会增加。

   栈空间:IPI 也是在内核栈上运行的。如果正在借用用户页表，请确保 ist (Interrupt Stack Table)
   配置正确，或者确保当前内核栈在所有页表中都是可见的。
   */
void ipi_send(uint8_t apic_id, uint8_t vector)
{
	// 1. 等待之前的 IPI 发送完成 (检查 Delivery Status 位)
	while (*(volatile uint32_t*)(LAPIC_BASE + LAPIC_ICR_LOW) & (1 << 12)) {
		/* spin */
	}

	// 2. 写入目标 APIC ID 到高 32 位
	*(volatile uint32_t*)(LAPIC_BASE + LAPIC_ICR_HIGH) = (uint32_t)apic_id << 24;

	// 3. 写入向量号和模式到低 32 位 (触发发送)
	// 0x4000 表示 Level Assert， 模式为 Fixed
	*(volatile uint32_t*)(LAPIC_BASE + LAPIC_ICR_LOW) = (uint32_t)vector | 0x4000;
}

// 发送给所有核心(排除自己)
void ipi_broadcast(uint8_t vector)
{
	L();
	while (*(volatile uint32_t*)(LAPIC_BASE + LAPIC_ICR_LOW) & (1 << 12)) {
		/* spin */
	}
	L();
	// Destination Shorthand: 0xC0000 表示 "All Excluding Self"
	*(volatile uint32_t*)(LAPIC_BASE + LAPIC_ICR_LOW) = 0xC0000 | (uint32_t)vector | 0x4000;

	L();
}
EXPORT_SYMBOL(ipi_broadcast);

/*
 * Logical CPU IDs are dense scheduler indices, while xAPIC IDs may be
 * sparse.  Until the x86 per-CPU context records its hardware APIC ID,
 * broadcast the wakeup and let only the CPU whose need_resched flag was
 * published act on it.  This is correct and keeps the mapping distinction
 * explicit; it can be replaced with a directed IPI as an optimization.
 */
void ipi_reschedule_cpu(uint32_t cpu_id)
{
	struct cpu_context *ctx;

	if (cpu_id >= MAX_CPUS)
		return;
	ctx = g_cpu_contexts[cpu_id];
	if (!ctx)
		return;

	__atomic_store_n(&ctx->need_resched, 1, __ATOMIC_RELEASE);
	if (ctx == cpu_get_ctx())
		return;
	ipi_broadcast(IPI_VECTOR_RESCHEDULE);
}

void ipi_reschedule_ack(void)
{
	struct cpu_context *ctx = cpu_get_ctx();

	if (ctx && ctx->id >= 0 && ctx->id < MAX_CPUS)
		__atomic_add_fetch(&g_reschedule_count[ctx->id], 1,
				   __ATOMIC_RELEASE);
}

uint64_t ipi_reschedule_count(uint32_t cpu_id)
{
	if (cpu_id >= MAX_CPUS)
		return 0;
	return __atomic_load_n(&g_reschedule_count[cpu_id],
			       __ATOMIC_ACQUIRE);
}
