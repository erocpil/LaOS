/*
 * ist.c - IST (Interrupt Stack Table) 静态栈分配与 TSS 装配
 *
 * 详见 ist.h.本文件负责：
 *   1. 静态分配 per-CPU x per-slot 的 IST 栈(BSS，16 页对齐)
 *   2. 提供 ist_init_cpu() 供 gdt_init_cpu() 调用
 *   3. 提供 ist_stack_top() 让 IDT 层查询(如果未来 vector 需要独立栈)
 *
 * 布局:ist_stacks[cpu_id][slot][byte]
 *   MAX_CPUS x NR_IST_SLOTS x IST_STACK_SIZE
 *   = 16 x 4 x 8192 = 512 KB BSS
 *   真实 4-CPU SMP 只用 4 x 3 x 8KB = 96 KB，其余 zero-fill 不占 RSS.
 */
#include "ist.h"
#include "gdt.h"
#include "define.h"
#include "log.h"
#include "debug.h"
#include <stdint.h>

extern struct tss_entry per_cpu_tss[MAX_CPUS];

/*
 * 16 字节对齐足够(x86_64 SysV ABI 要求 rsp 16 对齐进入函数)；
 * 但整栈按页对齐更清晰，且便于未来加 guard page.
 */
static uint8_t ist_stacks[MAX_CPUS][NR_IST_SLOTS][IST_STACK_SIZE]
    __attribute__((aligned(4096)));

uint64_t ist_stack_top(int cpu_id, int slot)
{
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) return 0;
    if (slot < 0 || slot >= NR_IST_SLOTS) return 0;
    // 栈向下增长，返回栈顶(末端 + 1)
    return (uint64_t)&ist_stacks[cpu_id][slot][IST_STACK_SIZE];
}

void ist_init_cpu(int cpu_id)
{
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
        L("[ist] invalid cpu_id %d", cpu_id);
        return;
    }

    struct tss_entry *tss = &per_cpu_tss[cpu_id];

    // slot 0 保持 0(tss memset 已归零)；1/2/3 填入栈顶
    tss->ist[0] = 0;                                        // 保留
    tss->ist[IST_SLOT_DF - 1]  = ist_stack_top(cpu_id, IST_SLOT_DF);
    tss->ist[IST_SLOT_NMI - 1] = ist_stack_top(cpu_id, IST_SLOT_NMI);
    tss->ist[IST_SLOT_MCE - 1] = ist_stack_top(cpu_id, IST_SLOT_MCE);
    tss->ist[3] = 0;
    tss->ist[4] = 0;
    tss->ist[5] = 0;
    tss->ist[6] = 0;

    L("[ist] cpu %d: DF=%p NMI=%p MCE=%p",
        cpu_id,
        (void*)tss->ist[IST_SLOT_DF - 1],
        (void*)tss->ist[IST_SLOT_NMI - 1],
        (void*)tss->ist[IST_SLOT_MCE - 1]);
}
