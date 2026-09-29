/*
 * gdt.c - GDT 初始化与 per-CPU TSS 装配
 *
 * 每 CPU 独立 GDT 副本(含各自的 TSS 段描述符).
 * gdt_init_cpu 汇编 TSS entry 并填入 IST 栈指针。
 */

#include "gdt.h"
#include "ist.h"
#include "heap.h"
#include "lapic.h"
#include "printf.h"
#include "string.h"
#include "define.h"
#include "debug.h"
#include "hhdm.h"
#include "log.h"

// 修改 gdt_set_tss，使其能将特定的 TSS 填入 GDT 的指定位置
// 注意：在多核模式下，一种简单的做法是每个核有自己的 GDT 副本，
// 或者在全局 GDT 中为每个核预留不同的 TSS 槽位。
// 这里推荐每个核拥有独立的 GDT 副本，这样代码逻辑最清晰。
struct gdt_entry **per_cpu_gdt = NULL;
struct gdt_ptr *per_cpu_gdt_ptr = NULL;
// 每个 CPU 独享一个 TSS 结构
struct tss_entry per_cpu_tss[MAX_CPUS];

/* P0-1: BSP bootstrap GDT — lives in BSS so it persists after gdt_init_cpu
 * returns.  Replaced later by gdt_init_dynamic() once kheap is online. */
static struct gdt_entry g_bsp_gdt[GDT_SIZE] __attribute__((aligned(16)));
static struct gdt_ptr g_bsp_gdt_ptr;

// 现在 GDT 需要更多槽位：
// 0:NULL, 1:K-Code, 2:K-Data, 3:U-Code, 4:U-Data, 5-6:TSS (占2个)
// struct gdt_entry gdt[7];
// struct gdt_ptr gdt_record;
// struct tss_entry kernel_tss;

// 告诉编译器 gdt_reload 是在汇编里定义的
extern void gdt_reload(struct gdt_ptr* ptr);
extern void tss_load();

uint64_t get_per_cpu_tss_rsp0(uint32_t cpu)
{
	return per_cpu_tss[cpu].rsp0;
}

void gdt_set_entry_cpu(struct gdt_entry *gdt, int num, uint64_t base, uint32_t limit,
		uint8_t access, uint8_t gran)
{
	gdt[num].base_low = (base & 0xFFFF);
	gdt[num].base_mid = (base >> 16) & 0xFF;
	gdt[num].base_high = (base >> 24) & 0xFF;

	gdt[num].limit_low = (limit & 0xFFFF);
	gdt[num].granularity = (limit >> 16) & 0x0F;

	gdt[num].granularity |= gran & 0xF0;
	gdt[num].access = access;
}

void gdt_set_tss_ext(struct gdt_entry* target_gdt, int num, uint64_t base, uint32_t limit)
{
	uint8_t *target = (uint8_t*)&target_gdt[num];
	memset(target, 0, 16);
	target[0] = limit & 0xFF;
	target[1] = (limit >> 8) & 0xFF;
	target[2] = base & 0xFF;
	target[3] = (base >> 8) & 0xFF;
	target[4] = (base >> 16) & 0xFF;
	target[5] = 0x89; // Present, 64-bit TSS
	target[6] = (limit >> 16) & 0x0F;
	target[7] = (base >> 24) & 0xFF;
	*((uint32_t*)&target[8]) = (base >> 32) & 0xFFFFFFFF;
}

void gdt_init_cpu(int cpu_id, uint64_t kernel_stack)
{
	struct gdt_entry *gdt = g_bsp_gdt;

	// 初始化该核的 GDT
	memset(gdt, 0, sizeof(struct gdt_entry) * GDT_SIZE);

	// 0: Null, 1: K-Code, 2: K-Data, 3: U-Code, 4: U-Data
	gdt_set_entry_cpu(gdt, 0, 0, 0, 0, 0);                // Null
	gdt_set_entry_cpu(gdt, 1, 0, 0xFFFFFFFF, 0x9A, 0xAF); // Kernel Code (注意 0xAF 包含 L 位)
	gdt_set_entry_cpu(gdt, 2, 0, 0xFFFFFFFF, 0x92, 0xAF); // Kernel Data CF -> AF
	gdt_set_entry_cpu(gdt, 3, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User Data
	gdt_set_entry_cpu(gdt, 4, 0, 0xFFFFFFFF, 0xFA, 0xAF); // User Code

	struct tss_entry* tss = &per_cpu_tss[cpu_id]; // 配置该核的专属 TSS
	memset(tss, 0, sizeof(struct tss_entry));
	// 指向该核专属的内核栈顶
	tss->rsp0 = kernel_stack;
	tss->iopb_offset = sizeof(struct tss_entry);

	// 装配 IST 栈(#DF / NMI / #MC 走独立栈，避免栈溢出触发 triple-fault)
	ist_init_cpu(cpu_id);

	// 将 TSS 填入该核 GDT 的 5-6 槽位
	uint64_t tss_base = (uint64_t)tss;
	gdt_set_tss_ext(gdt, 5, tss_base, sizeof(struct tss_entry) - 1);

	// 设置该核的 GDT 指针
	g_bsp_gdt_ptr.limit = (sizeof(struct gdt_entry) * 7) - 1;
	g_bsp_gdt_ptr.base = (uint64_t)gdt;

	gdt_reload(&g_bsp_gdt_ptr);

	struct gdt_ptr gdtr;
	asm volatile("sgdt %0" : "=m"(gdtr));

	struct gdt_ptr verify;
	asm volatile("sgdt %0" : "=m"(verify));

	tss_load(); // 告诉 CPU 使用 TSS 0x28

	L_TAG(LOG_BOOT, "GDT reloaded.\n");
}

// 建议放在 heap 初始化之后，线程系统初始化之前
void gdt_init_dynamic(uint32_t n)
{
	// 分配 per-cpu GDT 指针数组(如果还没分配)
	if (!per_cpu_gdt) {
		L("alloc per_cpu_gdt for %u cpus\n", n);
		if (n > MAX_CPUS) {
			kprintf("Warning %u max cpus (< %u) supported\n", MAX_CPUS, n);
			n = MAX_CPUS;
		}
		per_cpu_gdt = kmalloc(sizeof(struct gdt_entry*) * MAX_CPUS);
		per_cpu_gdt_ptr = kmalloc(sizeof(struct gdt_ptr) * MAX_CPUS);
		if (!per_cpu_gdt || !per_cpu_gdt_ptr) {
			panic("Failed to allocate per-cpu GDT arrays");
		}

		for (uint32_t cpu = 0; cpu < n; cpu++) {
			// 分配每个 CPU 的 GDT 表(建议多分配一些槽位以防未来扩展)
			per_cpu_gdt[cpu] = kmalloc(sizeof(struct gdt_entry) * GDT_SIZE);
			if (!per_cpu_gdt[cpu]) {
				panic("Failed to allocate GDT for cpu");
			}

			memset(per_cpu_gdt[cpu], 0, sizeof(struct gdt_entry) * GDT_SIZE);

			// 填充内容(复用现在的初始化逻辑)
			struct gdt_entry *gdt = per_cpu_gdt[cpu];

			gdt_set_entry_cpu(gdt, 0, 0, 0, 0, 0);                // Null
			gdt_set_entry_cpu(gdt, 1, 0, 0xFFFFFFFF, 0x9A, 0xAF); // Kernel Code
			gdt_set_entry_cpu(gdt, 2, 0, 0xFFFFFFFF, 0x92, 0xAF); // Kernel Data
			gdt_set_entry_cpu(gdt, 3, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User Data
			gdt_set_entry_cpu(gdt, 4, 0, 0xFFFFFFFF, 0xFA, 0xAF); // User Code

			// TSS(每个 CPU 独立)
			struct tss_entry *tss = &per_cpu_tss[cpu];
			// 保持原来的 TSS 初始化代码
			gdt_set_tss_ext(gdt, 5, (uint64_t)tss, sizeof(struct tss_entry) - 1);

			// 设置 GDTR
			per_cpu_gdt_ptr[cpu].limit = (sizeof(struct gdt_entry) * 7) - 1;
			per_cpu_gdt_ptr[cpu].base = (uint64_t)per_cpu_gdt[cpu];

			L("CPU %d dynamic GDT allocated at %p, base=%p\n",
					cpu, per_cpu_gdt[cpu], per_cpu_gdt_ptr[cpu].base);
		}
	}

	// 立即为当前 CPU 加载新的 GDT
	uint32_t current_cpu = get_cpu_id();
	gdt_reload(&per_cpu_gdt_ptr[current_cpu]);
	tss_load();  // 重新加载 TR

	// 验证
	struct gdt_ptr verify;
	asm volatile("sgdt %0" : "=m"(verify));
	L("After dynamic reload - GDTR base=%p limit=%d\n", verify.base, verify.limit);

	L("Testing BSP LAPIC access...\n");
	volatile uint32_t* test_eoi = (uint32_t*)phys_to_virt(0xFEE00000 + 0xB0);
	*test_eoi = 0; // 如果这里不崩，说明 VMM 映射 100% 成功。
	L("VMM: LAPIC Access Test Passed.\n");

	static uint32_t k = 0;
	if (++k == g_cpu_count) {
		L_TAG(LOG_BOOT, "GDT reloaded.\n");
	}
}

// 设置 TSS 中的内核栈顶
void tss_set_rsp0(uint32_t cpu_id, uint64_t rsp0)
{
	if (cpu_id < MAX_CPUS) {
		per_cpu_tss[cpu_id].rsp0 = rsp0;
	}
}
