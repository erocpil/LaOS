/*
 * x86_64 SYSCALL MSR 配置与系统调用分发。
 *
 * 配置 MSR_STAR/LSTAR/SFMASK，注册 syscall 入口。
 * 当前仅提供基本的用户态系统调用路由。
 */
#include <stdint.h>
#include <stddef.h>

#include "cpu.h"
#include "define.h"
#include "printf.h"
#include "sched.h"
#include "thread.h"
#include "syscall.h"
#include "log.h"
#include "timer.h"
#include "vma.h"
#include "vmm.h"
#include "pmm.h"
#include "hhdm.h"
#include "asm_offsets_gas.h"
#include "gdt.h"

void syscall_init()
{
	/* 启用 EFER.SCE（System Call Extensions）。 */
	uint64_t efer = rdmsr(MSR_EFER);
	wrmsr(MSR_EFER, efer | 1);

	/* 配置 STAR 中的内核选择子及 SYSRET 用户选择子基数。长模式 SYSRET
	 * 将基数加 16 装入 CS、加 8 装入 SS，对应 USER_CS 和 USER_SS。 */
	uint64_t star = ((uint64_t)0x13 << 48) | ((uint64_t)0x08 << 32);
	wrmsr(MSR_STAR, star);

	/* 设置 64 位系统调用入口。 */
	extern void syscall_entry();
	wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* SYSCALL 入口清除 RFLAGS.IF 和 RFLAGS.TF，先以关中断状态进入内核。 */
	wrmsr(MSR_SFMASK, 0x202);

	static uint32_t k = 0;
	if (++k == g_cpu_count) {
		L_TAG(LOG_SYSCALL, "syscall initialized.\n");
	}
}

static int64_t sys_write(int fd, const char *buf, size_t count)
{
	/* 当前仅支持 stdout 和 stderr。 */
	if (fd != 1 && fd != 2) {
		return -1;
	}

	/* 仅接受低半区用户地址。失败时不输出日志，避免无效请求灌满内核日志。 */
	if ((uintptr_t)buf >= 0xFFFF800000000000) {
		return -1;
	}

	/* 防止区间加法溢出、越过用户空间边界，并限制单次写入长度。 */
	if (count == 0) {
		return 0;
	}
	if (count > PAGE_SIZE) {
		return -1;
	}
	if ((uintptr_t)buf + count < (uintptr_t)buf ||
		(uintptr_t)buf + count >= 0xFFFF800000000000) {
		return -1;
	}

	/* 系统调用入口保持当前 CR3；线程页表同时映射内核区和用户区。将用户
	 * 数据分块复制到有界内核栈缓冲区，再作为字符串输出，避免把用户内容
	 * 当作格式串解释。 */
	enum { SYS_WRITE_CHUNK = 256 };
	char tmp[SYS_WRITE_CHUNK];
	size_t off = 0;
	while (off < count) {
		size_t take = count - off;
		if (take > SYS_WRITE_CHUNK - 1) {
			take = SYS_WRITE_CHUNK - 1;
		}
		for (size_t i = 0; i < take; i++) {
			tmp[i] = buf[off + i];
		}
		tmp[take] = '\0';
		kprintf("%s", tmp);
		off += take;
	}

	return count; /* 返回已处理的字节数。 */
}

/* 将毫秒换算为 tick 后调用 schedule_timeout()。
 *
 * 1 tick = 10ms (TIMER_HZ=100)，最小睡眠 1 tick。
 * msec == 0 等价于 schedule() 让出 CPU，不设超时。 */
static inline uint64_t sys_msleep(uint64_t msec)
{
	if (msec == 0) {
		schedule();
		return 0;
	}

	uint64_t ticks = (msec * TIMER_HZ) / 1000;
	if (ticks == 0) {
		ticks = 1; /* 不足 10ms 按 1 tick 处理 */
	}

	return (uint64_t)schedule_timeout(ticks);
}

static inline void sys_exit(struct trap_frame *regs)
{
	L("CPU %d Thread %s %ld requested exit with code %ld", cpu_get_ctx()->id,
		get_current()->name, get_current()->id, regs->rdi);
	/* 与入口 sti 对称：线程即将死亡，无需被抢占。提前关中断让
	 * thread_exit 在 IF=0 下进入，与原版(SFMASK 屏蔽 IF 后直入
	 * thread_exit)语义一致；不依赖 __builtin_unreachable 抑制
	 * 死代码，也不依赖 thread_exit 内部的 cli 时序。 */
	asm volatile("cli");
	thread_exit((int)regs->rdi);
}

/* C 层系统调用分发器。入口先开放 IRQ，允许系统调用执行期间被时钟中断
 * 抢占；退出前关闭 IRQ 并检查 need_resched。系统调用栈由 syscall 入口
 * 单独构造，不能直接复用中断返回路径，因此退出调度在本函数中处理。
 * 会阻塞或主动让出的具体系统调用在各自实现中完成调度；普通调用仅在
 * 退出检查发现待调度请求时切换线程。 */
/* ---- mmap / munmap helpers ---- */

static uint64_t do_mmap(uint64_t length, uint64_t prot, uint64_t mmap_flags)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *t = ctx->current;
	if (!t || !t->pml4_phys) {
		return 0;
	}

	/* 向上取整到页边界。 */
	const uint64_t page_mask = PAGE_SIZE - 1;
	if (length > UINT64_MAX - page_mask) {
		return 0;
	}
	length = (length + page_mask) & ~page_mask;
	if (length == 0) {
		return 0;
	}

	/* 查找空闲用户虚拟地址区间。 */
	uint64_t vaddr = vma_find_free(length);
	if (!vaddr || vaddr > UINT64_MAX - length) {
		return 0;
	}

	/* 先登记 VMA，按需分页路径依赖该记录验证地址。 */
	if (!vma_alloc(t, vaddr, vaddr + length, (uint32_t)prot,
		    MAP_ANONYMOUS | MAP_PRIVATE | (uint32_t)mmap_flags)) {
		return 0;
	}

	/* 延迟映射仅登记 VMA，物理页由缺页处理器按需分配。 */
	if (mmap_flags & MAP_LAZY) {
		return vaddr;
	}

	/* 立即分配物理页并建立映射。 */
	uint64_t flags = PTE_PRESENT | PTE_USER;
	if (prot & PROT_WRITE) {
		flags |= PTE_WRITABLE;
	}
	if (!(prot & PROT_EXEC)) {
		flags |= PTE_NX;
	}

	uint64_t cursor = vaddr;
	uint64_t remaining = length;
	while (remaining > 0) {
		void *phys = pmm_alloc();
		if (!phys) {
			goto rollback;
		}
		if (vmm_map_user((uint64_t *)t->pml4_phys, cursor, (uint64_t)(uintptr_t)phys,
			    flags) != 0) {
			pmm_free(phys);
			goto rollback;
		}
		cursor += PAGE_SIZE;
		remaining -= PAGE_SIZE;
	}

	return vaddr;

rollback:
	/* 失败映射之前的页已提交到用户页表；逐页撤销映射并归还物理页。 */
	for (uint64_t va = vaddr; va < cursor; va += PAGE_SIZE) {
		uint64_t phys = vmm_get_phys((uint64_t)t->pml4_phys, va) & PTE_ADDR_MASK;
		vmm_unmap((uint64_t *)phys_to_virt((uint64_t)t->pml4_phys), va);
		if (phys) {
			pmm_free((void *)(uintptr_t)phys);
		}
	}
	vma_free(t, vaddr, length);

	return 0;
}

static int do_munmap(uint64_t addr, uint64_t length)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *t = ctx->current;
	if (!t || !t->pml4_phys) {
		return -1;
	}

	/* 向上取整到页边界。 */
	const uint64_t page_mask = PAGE_SIZE - 1;
	length = (length + page_mask) & ~page_mask;
	if (length == 0) {
		return -1;
	}
	addr &= ~page_mask;

	/* 要求待解除映射区间与单个 VMA 完全一致。 */
	struct vma *v = vma_find(t, addr);
	if (length > UINT64_MAX - addr) {
		return -1;
	}
	if (!v || v->start != addr || v->end != addr + length) {
		return -1;
	}

	/* 逐页解除映射并释放对应物理页。 */
	for (uint64_t va = addr; va < addr + length; va += PAGE_SIZE) {
		uint64_t raw = vmm_get_phys((uint64_t)(uintptr_t)t->pml4_phys, va);
		uint64_t phys = raw & PTE_ADDR_MASK;
		vmm_unmap((uint64_t *)phys_to_virt((uint64_t)(uintptr_t)t->pml4_phys), va);
		if (phys) {
			pmm_free((void *)(uintptr_t)phys);
		}
	}

	/* 从线程的 VMA 列表中移除该区间。 */
	return vma_free(t, addr, length);
}

void syscall_handler(struct trap_frame *regs)
{
	asm volatile("sti");

	int64_t ret = -1;
	uint64_t syscall_num = regs->rax;

	switch (syscall_num) {
		case SYS_WRITE: // 1
			ret = sys_write((int)regs->rdi, (const char *)regs->rsi, (size_t)regs->rdx);
			break;
		case SYS_SLEEP: // 35
			ret = (int64_t)sys_msleep(regs->rdi);
			break;
		case SYS_YIELD: // 3
			schedule();
			ret = 0;
			break;
		case SYS_EXIT: // 60
			sys_exit(regs);
			__builtin_unreachable();
		case SYS_MMAP: {
			// 5: mmap(length=rdi, prot=rsi, flags=rdx)
			uint64_t length = regs->rdi;
			uint64_t prot = regs->rsi;
			uint64_t map_flags = regs->rdx;
			ret = (int64_t)do_mmap(length, prot, map_flags);
			break;
		}
		case SYS_MUNMAP: {
			// 6: munmap(addr=rdi, length=rsi)
			uint64_t addr = regs->rdi;
			uint64_t length = regs->rsi;
			ret = (int64_t)do_munmap(addr, length);
			break;
		}
		default:
			kprintf("Unknown syscall: %lu\n", syscall_num);
			break;
	}

	regs->rax = ret;

	/* 抢占循环：处理"切回本线程时发现 CPU 上有新 resched"的窗口。
	 *
	 * 时序:schedule() -> __schedule 切到 next -> next 跑期间被时钟抢/
	 * 主动让出 -> ...... -> 调度器最终切回当前线程 -> schedule 返回。
	 * next 跑的这段时间里，本 CPU 的 need_resched(per-CPU 标志)可能
	 * 又被设置(next 的时钟抢占路径或别处置位).单次 if 会漏掉这次
	 * 信号，要等下一次时钟才兑现，延迟一个 tick；while 把所有累积的
	 * resched 消化完再 sysret 返回用户。
	 *
	 * IF 状态：进入循环时 cli，IF=0;schedule 内部 __schedule 用
	 * save_and_disable_interrupts/restore_interrupts 配对，进入时保存
	 * 的 flags=IF=0,restore 后仍 IF=0.整个循环全程关中断，与
	 * Linux preempt_schedule_irq 的"do { ... } while (need_resched())"
	 * 同构(差异:Linux 在 schedule 调用前后 local_irq_enable/disable
	 * 让 schedule 体可被中断；LaOS __schedule 自己关中断保护 switch，
	 * 这里无需额外开关).
	 */
	asm volatile("cli");
	int loops = 0;
	while (check_need_schedule()) {
		schedule();
		loops++;
	}
	/* 钩子：只在循环跑 >1 次时打印：这正是"切回时新增 resched"窗口被
	 * 抢占循环捕获的现场证据.loops==0 或 1 是常态(前者没人抢，后者
	 * 是单次抢占)，不打印避免刷屏。 */
	if (loops > 1) {
		struct cpu_context *_c = cpu_get_ctx();
		L("[SYSCALL PREEMPT LOOP] CPU %d thr=%s no=%lu loops=%d", _c->id, _c->current->name,
			syscall_num, loops);
	}
}

/* arch_user_thread_entry_stub -- x86_64: construct iretq frame and jump to Ring 3 */
void arch_user_thread_entry_stub(struct thread *t)
{
	arch_local_irq_disable();

	tss_set_rsp0(cpu_get_ctx()->id, (uint64_t)t->kernel_stack);

	uint64_t stack_top = (uint64_t)t->kernel_stack_base + KERNEL_STACK_SIZE;
	stack_top &= ~0xfULL;

	L("[stub] going to user, entry=%p usp=%p\n", t->entry_point, t->user_stack);

	/* fill iretq frame -- no function calls after this */
	uint64_t *rsp = (uint64_t *)stack_top;
	rsp -= 5;
	rsp[0] = t->entry_point;	  /* RIP  */
	rsp[1] = USER_CS_SEL;		  /* CS   */
	rsp[2] = 0x202;			  /* RFLAGS (IF=1) */
	rsp[3] = (uint64_t)t->user_stack; /* RSP3 */
	rsp[4] = USER_SS_SEL;		  /* SS3  */

	arch_enter_usermode(rsp);
}
