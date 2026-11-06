/*
 * x86_64 GDT、TSS 与每 CPU 特权级栈配置。
 *
 * BSP 先使用静态 GDT，堆可用后切换到独立的每 CPU GDT；TSS 按 CPU 编号存储。
 * TSS 提供特权级切换所需的 RSP0 和异常 IST 栈顶，不用于硬件任务切换。
 * 动态 GDT 的共享存储由 BSP 在发布 AP 入口前分配，各 CPU 随后装载自身 GDTR/TR。
 */

#include "gdt.h"
#include "ist.h"
#include "heap.h"
#include "lapic.h"
#include "printf.h"
#include "string.h"
#include "define.h"
#include "debug.h"
#include "log.h"

/* 各 CPU 的 GDT 与 GDTR。 */
struct gdt_entry **per_cpu_gdt = NULL;
struct gdt_ptr *per_cpu_gdt_ptr = NULL;
/* 每个 CPU 独立的 TSS。 */
struct tss_entry per_cpu_tss[MAX_CPUS];

/* BSP 的静态引导 GDT；堆可用后由动态分配的 GDT 替换。 */
static struct gdt_entry g_bsp_gdt[GDT_SIZE] __attribute__((aligned(16)));
static struct gdt_ptr g_bsp_gdt_ptr;

/* 使用槽位 0～6；64 位 TSS 描述符占用槽位 5 和 6。 */
extern void gdt_reload(struct gdt_ptr *ptr);
extern void tss_load();

/* 调用方须保证 cpu 小于 MAX_CPUS；返回值为 TSS 中保存的线性栈地址。 */
uint64_t get_per_cpu_tss_rsp0(uint32_t cpu)
{
	return per_cpu_tss[cpu].rsp0;
}

/* 写入一个 8 字节代码段或数据段描述符。调用方保证槽位有效；limit 取低
 * 20 位，gran 仅提供标志位所在的高半字节，段限长的低半字节由 limit 填入。 */
void gdt_set_entry_cpu(
	struct gdt_entry *gdt, int num, uint64_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
	gdt[num].base_low = (base & 0xFFFF);
	gdt[num].base_mid = (base >> 16) & 0xFF;
	gdt[num].base_high = (base >> 24) & 0xFF;

	gdt[num].limit_low = (limit & 0xFFFF);
	gdt[num].granularity = (limit >> 16) & 0x0F;

	gdt[num].granularity |= gran & 0xF0;
	gdt[num].access = access;
}

/* 写入 16 字节的 TSS 描述符。base 为 TSS 的线性地址；调用方须为 num 和
 * num + 1 保留连续槽位，并传入以字节计的段限长（TSS 大小减一）。 */
void gdt_set_tss_ext(struct gdt_entry *target_gdt, int num, uint64_t base, uint32_t limit)
{
	uint8_t *target = (uint8_t *)&target_gdt[num];
	memset(target, 0, 16);
	target[0] = limit & 0xFF;
	target[1] = (limit >> 8) & 0xFF;
	target[2] = base & 0xFF;
	target[3] = (base >> 8) & 0xFF;
	target[4] = (base >> 16) & 0xFF;
	target[5] = 0x89; // P=1、DPL=0、类型为可用的 64 位 TSS；LTR 后由处理器置忙。
	target[6] = (limit >> 16) & 0x0F;
	target[7] = (base >> 24) & 0xFF;
	*((uint32_t *)&target[8]) = (base >> 32) & 0xFFFFFFFF;
}

/* BSP 早期启动入口：无需堆分配，使用共享的静态引导 GDT。
 * 在本地 IRQ 关闭时调用，cpu_id 须有效；不能由多个 CPU 并发调用。
 * 清零指定 TSS 后设置 RSP0、禁用 I/O 位图，并为该 CPU 装配 IST。 */
void gdt_init_cpu(int cpu_id, uint64_t kernel_stack)
{
	struct gdt_entry *gdt = g_bsp_gdt;

	memset(gdt, 0, sizeof(struct gdt_entry) * GDT_SIZE);

	/* 槽位顺序须与 USER_SS=0x1b、USER_CS=0x23 及 syscall 的选择子约定一致。
	 * access 的 DPL 位区分内核态和用户态；代码段的 L=1、D=0 选择 64 位模式。
	 * SYSCALL 使用内核 CS 基数及其后一个数据段；64 位 SYSRET 使用用户基数
	 * 加 8 的 SS 与加 16 的 CS，因此用户数据段排在用户代码段之前。 */
	// 空描述符，不作为有效段使用。
	gdt_set_entry_cpu(gdt, 0, 0, 0, 0, 0);
	// DPL 0 代码段：可执行、可读，L=1、D=0。
	gdt_set_entry_cpu(gdt, 1, 0, 0xFFFFFFFF, 0x9A, 0xAF);
	// DPL 0 数据段：可读写。
	gdt_set_entry_cpu(gdt, 2, 0, 0xFFFFFFFF, 0x92, 0xAF);
	// DPL 3 数据段：可读写，用户 SS 对应此槽位。
	gdt_set_entry_cpu(gdt, 3, 0, 0xFFFFFFFF, 0xF2, 0xCF);
	// DPL 3 代码段：可执行、可读，L=1、D=0，用户 CS 对应此槽位。
	gdt_set_entry_cpu(gdt, 4, 0, 0xFFFFFFFF, 0xFA, 0xAF);

	struct tss_entry *tss = &per_cpu_tss[cpu_id];
	memset(tss, 0, sizeof(struct tss_entry));
	// RSP0 用于从较低特权级进入 Ring 0；不保存当前执行流的 RSP。
	tss->rsp0 = kernel_stack;
	// 位图起点位于 TSS 段限长之外，表示未提供 I/O 权限位图。
	tss->iopb_offset = sizeof(struct tss_entry);

	// 为 #DF、NMI 和 #MC 配置独立 IST 栈，减少对被中断栈可用性的依赖。
	ist_init_cpu(cpu_id);

	// 将 TSS 填入该核 GDT 的 5-6 槽位
	uint64_t tss_base = (uint64_t)tss;
	gdt_set_tss_ext(gdt, 5, tss_base, sizeof(struct tss_entry) - 1);

	g_bsp_gdt_ptr.limit = (sizeof(struct gdt_entry) * 7) - 1;
	g_bsp_gdt_ptr.base = (uint64_t)gdt;

	gdt_reload(&g_bsp_gdt_ptr);

	struct gdt_ptr gdtr;
	asm volatile("sgdt %0" : "=m"(gdtr));

	struct gdt_ptr verify;
	asm volatile("sgdt %0" : "=m"(verify));

	tss_load(); // 将槽位 5 对应的 TSS 选择子 0x28 装入 TR。

	L_TAG(LOG_BOOT, "GDT reloaded.\n");
}

/* BSP 首次调用时按 n 分配各 CPU 的 GDT；此步骤必须在发布 AP 入口前完成。
 * 后续调用只装载当前 CPU 的 GDTR/TR，n 不再参与分配。当前以 get_cpu_id()
 * 返回值索引数组，调用方须保证该编号对应已分配槽位；本函数不提供并发初始化保护。
 * 本函数不清零 TSS，也不装配 IST；TSS 内容由相应 CPU 的初始化路径维护。 */
void gdt_init_dynamic(uint32_t n)
{
	if (!per_cpu_gdt) {
		L("alloc per_cpu_gdt for %u cpus\n", n);
		if (n > MAX_CPUS) {
			kprintf("Warning %u max cpus (< %u) supported\n", MAX_CPUS, n);
			n = MAX_CPUS;
		}
		per_cpu_gdt = kmalloc(sizeof(struct gdt_entry *) * MAX_CPUS);
		per_cpu_gdt_ptr = kmalloc(sizeof(struct gdt_ptr) * MAX_CPUS);
		if (!per_cpu_gdt || !per_cpu_gdt_ptr) {
			panic("Failed to allocate per-cpu GDT arrays");
		}

		for (uint32_t cpu = 0; cpu < n; cpu++) {
			per_cpu_gdt[cpu] = kmalloc(sizeof(struct gdt_entry) * GDT_SIZE);
			if (!per_cpu_gdt[cpu]) {
				panic("Failed to allocate GDT for cpu");
			}

			memset(per_cpu_gdt[cpu], 0, sizeof(struct gdt_entry) * GDT_SIZE);

			// 与静态引导 GDT 保持相同的段槽位布局。
			struct gdt_entry *gdt = per_cpu_gdt[cpu];

			// 空描述符，不作为有效段使用。
			gdt_set_entry_cpu(gdt, 0, 0, 0, 0, 0);
			// DPL 0 代码段：可执行、可读，L=1、D=0。
			gdt_set_entry_cpu(gdt, 1, 0, 0xFFFFFFFF, 0x9A, 0xAF);
			// DPL 0 数据段：可读写。
			gdt_set_entry_cpu(gdt, 2, 0, 0xFFFFFFFF, 0x92, 0xAF);
			// DPL 3 数据段：可读写，用户 SS 对应此槽位。
			gdt_set_entry_cpu(gdt, 3, 0, 0xFFFFFFFF, 0xF2, 0xCF);
			// DPL 3 代码段：可执行、可读，L=1、D=0，用户 CS 对应此槽位。
			gdt_set_entry_cpu(gdt, 4, 0, 0xFFFFFFFF, 0xFA, 0xAF);

			struct tss_entry *tss = &per_cpu_tss[cpu];
			// 描述符仅引用现有 TSS；AP 的 RSP0 随后由 per_cpu_init() 设置。
			gdt_set_tss_ext(gdt, 5, (uint64_t)tss, sizeof(struct tss_entry) - 1);

			per_cpu_gdt_ptr[cpu].limit = (sizeof(struct gdt_entry) * 7) - 1;
			per_cpu_gdt_ptr[cpu].base = (uint64_t)per_cpu_gdt[cpu];

			L("CPU %d dynamic GDT allocated at %p, base=%p\n", cpu, per_cpu_gdt[cpu],
				per_cpu_gdt_ptr[cpu].base);
		}
	}

	// GDTR 和 TR 属于本地处理器状态，每个 CPU 都须分别装载。
	uint32_t current_cpu = get_cpu_id();
	gdt_reload(&per_cpu_gdt_ptr[current_cpu]);
	tss_load(); // 重新加载 TR

	// 读回 GDTR 供日志观察；此处未比较其值，不构成自动校验。
	struct gdt_ptr verify;
	asm volatile("sgdt %0" : "=m"(verify));
	L("After dynamic reload - GDTR base=%p limit=%d\n", verify.base, verify.limit);

	static uint32_t gdt_loaded_count;
	uint32_t loaded =
		__atomic_add_fetch(&gdt_loaded_count, 1, __ATOMIC_RELAXED);
	if (loaded == g_cpu_count) {
		L_TAG(LOG_BOOT, "GDT reloaded.\n");
	}
}

/* 更新指定 CPU 的 TSS.RSP0。无效编号被忽略；不修改当前 RSP，也不更新 IST。
 * 调用方须协调同一 TSS 的并发访问，并在相应用户态入口生效前完成更新。 */
void tss_set_rsp0(uint32_t cpu_id, uint64_t rsp0)
{
	if (cpu_id < MAX_CPUS) {
		per_cpu_tss[cpu_id].rsp0 = rsp0;
	}
}
