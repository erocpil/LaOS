/*
 * page_fault.c — x86_64 页错误恢复 (P4-4)
 *
 * 拦截用户态 #PF (vector 14) 的 not-present 异常，按需分配
 * 物理页并映射到用户页表。地址不在 VMA 内或权限违规时
 * 终止用户进程而非 panic。
 *
 * x86_64 错误码 (error_code):
 *   bit 0 (P):   0=not-present, 1=protection violation
 *   bit 1 (W/R): 0=read, 1=write
 *   bit 2 (U/S): 0=kernel mode, 1=user mode
 *   bit 3 (RSVD): reserved bit violation
 *   bit 4 (I/D): 0=data fetch, 1=instruction fetch
 *   bit 5 (PK):  protection key violation
 *
 * 故障地址在 CR2 中。
 */

#include "page_fault.h"
#include "idt.h"
#include "cpu.h"
#include "thread.h"
#include "vma.h"
#include "vmm.h"
#include "pmm.h"
#include "hhdm.h"
#include "printf.h"
#include "sched.h"
#include "arch_irq.h"
#include "debug.h"

/* 杀死当前用户进程（不返回） */
static void page_fault_kill(struct thread *t, uint64_t cr2, uint64_t ec,
			    const char *reason)
{
	kprintf("\n[page_fault] killing thread %d (%s): %s\n"
		"  CR2=0x%016lx  error_code=0x%02lx\n",
		t->id, t->name, reason, cr2, ec);

	thread_set_status(t, THREAD_ZOMBIE);
	arch_local_irq_enable();
	schedule();
	panic("page_fault_kill: schedule() returned");
}

/* 按需分页：为 VMA 覆盖的地址分配物理页并映射。
 * 返回 0 成功，-1 失败（OOM 等）。 */
static int page_fault_demand(struct thread *t, uint64_t vaddr)
{
	/* 页对齐 */
	vaddr &= ~4095ULL;

	/* 检查是否已有映射（竞态：另一个线程可能已处理） */
	if (vmm_is_mapped((uint64_t)t->pml4_phys, vaddr))
		return 0;

	void *phys = pmm_alloc();
	if (!phys)
		return -1;

	/* 根据 VMA prot 决定 PTE 标志位。
	 * x86_64: PTE_USER(bit2) 控制 Ring 3 访问；
	 * PTE_WRITABLE(bit1)=0 表示只读。 */
	struct vma *v = vma_find(t, vaddr);
	uint64_t flags = PTE_PRESENT | PTE_USER;
	if (v && (v->prot & PROT_WRITE))
		flags |= PTE_WRITABLE;
	if (v && !(v->prot & PROT_EXEC))
		flags |= PTE_NX;

	if (vmm_map_user((uint64_t *)t->pml4_phys, vaddr,
			 (uint64_t)(uintptr_t)phys, flags) != 0) {
		pmm_free(phys);
		return -1;
	}

	return 0;
}

void page_fault_handler(struct interrupt_frame *frame)
{
	/* CR2 必须在最开始读，越早越好 */
	uint64_t cr2 = arch_read_cr2();
	uint64_t ec  = frame->error_code;

	/* 只处理用户态异常（bit 2 = U/S） */
	if (!(ec & 0x4)) {
		/* 内核态 #PF — 让其落入 exception_handler */
		exception_handler(frame);
		return;
	}

	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *t = ctx ? ctx->current : NULL;

	/* 无当前线程（极早期异常）→ panic */
	if (!t) {
		kprintf("[page_fault] no current thread at CR2=0x%lx, ec=0x%lx\n",
			cr2, ec);
		exception_handler(frame);
		return;
	}

	/* 内核线程无 user VMA → kill */
	if (!t->is_user || !t->vma_list) {
		page_fault_kill(t, cr2, ec, "kernel thread / no VMA");
	}

	/* 检查 VMA */
	struct vma *v = vma_find(t, cr2);
	if (!v) {
		page_fault_kill(t, cr2, ec, "address not in any VMA");
	}

	/*
	 * 按 error_code 位分类处理：
	 *
	 * P=0 (not-present): 页表项不存在 → 按需分页。
	 * P=1 (protection): 映射存在但权限不匹配 → 无 COW，kill。
	 * RSVD=1: 保留位违规 → kill。
	 */
	if (!(ec & 1)) {
		/* Not-present fault — demand page */
		if (page_fault_demand(t, cr2) != 0)
			page_fault_kill(t, cr2, ec, "demand paging OOM");
		return;
	}

	if (ec & 8) {
		/* Reserved bit violation */
		page_fault_kill(t, cr2, ec, "reserved bit violation");
	}

	/* Protection violation */
	const char *access_type = (ec & 0x10) ? "instruction fetch"
		: ((ec & 2) ? "write" : "read");
	kprintf("[page_fault] protection fault: %s at 0x%lx"
		" (VMA prot=0x%x)\n",
		access_type, cr2, v->prot);
	page_fault_kill(t, cr2, ec, "protection violation");
}
