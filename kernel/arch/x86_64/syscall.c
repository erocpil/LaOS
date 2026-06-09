/*
 * syscall.c - x86_64 syscall(LSTAR)初始化与分发
 *
 * 配置 MSR_STAR/LSTAR/SFMASK，注册 syscall 入口。
 * 当前仅提供基本的用户态系统调用路由。
 */
#include <stdint.h>
#include <stddef.h>

#include "cpu.h"
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
	// 1. 开启 EFER 中的 SCE (System Call Extensions)
	uint64_t efer = rdmsr(MSR_EFER);
	wrmsr(MSR_EFER, efer | 1);

	// 2. 设置 STAR: 定义段选择子基数
	// STAR[47:32] = 内核 CS (0x08)；sysret 后内核 SS = 0x08 + 8 = 0x10
	// STAR[63:48] = 用户 CS 基数 (0x13)；long mode sysret 用 base+16 装入 CS，
	//   base+8 装入 SS，因此实际加载 CS = 0x23 (USER_CS),SS = 0x1b (USER_SS).
	uint64_t star = ((uint64_t)0x13 << 48) | ((uint64_t)0x08 << 32);
	wrmsr(MSR_STAR, star);

	// 3. 设置 LSTAR: 系统调用入口点
	extern void syscall_entry();
	wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

	// 4. 设置 SFMASK: 屏蔽 RFLAGS 位
	// 0x202 屏蔽 IF (中断) 和 TF (陷阱)，确保进入内核时是关中断状态
	wrmsr(MSR_SFMASK, 0x202);

	static uint32_t k = 0;
	if (++k == g_cpu_count) {
		L_TAG(LOG_SYSCALL, "syscall initialized.\n");
	}
}

static int64_t sys_write(int fd, const char* buf, size_t count)
{
	// 目前仅支持 stdout/stderr
	if (fd != 1 && fd != 2) {
		return -1;
	}

	// 安全检查:buf 必须完全位于用户空间(低半区).
	// 高半区起点 0xFFFF800000000000:目前是裸字面量，跨多个文件用，待后续抽
	// 全局 KERNEL_BASE 常量(heap.c / idt.c / main.c 同样字面量).
	// 失败路径不打日志：用户态恶意 buf 会高频触发，避免内核日志被灌爆构成 DoS 通道。
	if ((uintptr_t)buf >= 0xFFFF800000000000) {
		return -1;
	}

	/* P0-4: guard against overflow and kernel-space wrap.
	 * buf is valid user-space, but buf+count could overflow or cross
	 * the kernel boundary.  Also cap count to a reasonable max. */
	if (count == 0)
		return 0;
	if (count > 4096)
		return -1;
	if ((uintptr_t)buf + count < (uintptr_t)buf
	    || (uintptr_t)buf + count >= 0xFFFF800000000000) {
		return -1;
	}

	/* 输出用户 buf.前置条件：
	 *  - syscall_entry 没切 CR3，当前线程 PML4 高位含内核映射，低位是用户映射，
	 *    因此 buf(用户低地址)在本上下文直接可读。
	 *  - 上面安检确认 buf 不进内核高地址，越界 / 内核地址直接拒。
	 *
	 * 实现：
	 *  - 拷到内核栈临时缓冲，截到 SYS_WRITE_CHUNK - 1 防爆栈；
	 *  - 末尾补 NUL，再 kprintf("%s"， tmp):绝不把用户 buf 当 fmt 串，
	 *    否则用户 buf 里的 '%c' '%s' 会被 vsnprintf 解析读越界。
	 *  - 长 buf 分块循环，每块独占一次 kprintf(即一次 print_lock).
	 */
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

	return count; // 返回实际写入的字节数
}

/* SYS_SLEEP: 将毫秒转换为 tick 后调 schedule_timeout()。
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
	if (ticks == 0)
		ticks = 1;               /* 不足 10ms 按 1 tick 处理 */
	return (uint64_t)schedule_timeout(ticks);
}

static inline void sys_exit(struct trap_frame *regs)
{
	L("CPU %d Thread %s %ld requested exit with code %ld",
			cpu_get_ctx()->id, get_current()->name, get_current()->id, regs->rdi);
	/* 与入口 sti 对称：线程即将死亡，无需被抢占。提前关中断让
	 * thread_exit 在 IF=0 下进入，与原版(SFMASK 屏蔽 IF 后直入
	 * thread_exit)语义一致；不依赖 __builtin_unreachable 抑制
	 * 死代码，也不依赖 thread_exit 内部的 cli 时序。 */
	asm volatile("cli");
	thread_exit((int)regs->rdi);
}

/* syscall_handler:syscall 指令进入的 C 层分发器。
 *
 * 抢占模型(激进路):syscall 体内允许时钟中断打入并触发抢占。
 *
 *  入口 sti:syscall 指令进入时 SFMASK=0x202 保证 IF=0，TF=0.此处显式
 *  开中断，时钟可在 syscall 期间打入；IRQ 返回路径的 check_need_schedule
 *  会按需 __schedule_irq 抢走当前 syscall 上下文。
 *
 *  出口 cli + check_need_schedule + schedule:
 *    1. cli 关中断，避免 check_need_schedule 与 sysret 之间被嵌套抢占；
 *    2. 检查 need_resched，由叶子 schedule()(非 __schedule_irq)兑现：
 *       本路径栈布局是 syscall_entry 压的 trap_frame，最终走 sysret 而非
 *       iretq，不能复用 IRQ 返回路径。
 *
 *  锁安全性:kprintf 等热路径用 spin_lock_irqsave/restore，持锁段始终
 *  关中断，不会被时钟抢占，无死锁风险。
 *
 *  内核栈深度:KERNEL_STACK_SIZE = 16 * PAGE_SIZE = 64KB;trap_frame
 *  ~120B + 嵌套 interrupt_frame ~152B + C 帧，5-6 层嵌套抢占无压力。
 *
 * 调度策略:syscall 后是否让出 CPU 由各 case + 出口检查共同决定，
 * 分发器不再无条件 schedule():
 *  - SYS_WRITE:返回用户态继续跑(fast path).原版每次 write 后强制
 *    schedule,user main 循环 msleep(10)+write 实际变成 sleep+yield 双重
 *    让出，吞吐减半。
 *  - SYS_SLEEP:sys_msleep 内部已 schedule_timeout 让出；出口 check
 *    幂等返 0(need_resched 已被 __schedule 清零).
 *  - SYS_EXIT:thread_exit 永不返回，下面的代码不会执行。
 *  - default:未知 syscall，记录后返回 -1.
 *
 * 这种"分发器不主动 schedule，由叶子调用 + 出口检查兑现"是 Linux 风格：
* SYSCALL 快路径力求最短，抢占由 ret-to-user 时钟中断或显式 yield 触发。
	*/
/* ---- mmap / munmap helpers ---- */

static uint64_t do_mmap(uint64_t length, uint64_t prot, uint64_t mmap_flags)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *t = ctx->current;
	if (!t || !t->pml4_phys) return 0;

	/* Round up to page size */
	length = (length + 4095) & ~4095ULL;
	if (length == 0) return 0;

	/* Find free virtual address range */
	uint64_t vaddr = vma_find_free(length);
	if (!vaddr) return 0;

	/* Track in VMA list first (demand-paging relies on VMA lookup) */
	if (!vma_alloc(t, vaddr, vaddr + length, (uint32_t)prot,
		       MAP_ANONYMOUS | MAP_PRIVATE | (uint32_t)mmap_flags)) {
		return 0;
	}

	/* Lazy mapping: VMA only, pages allocated on demand by page_fault_handler */
	if (mmap_flags & MAP_LAZY)
		return vaddr;

	/* Eager allocation: allocate physical pages and map */
	uint64_t flags = PTE_PRESENT | PTE_USER;
	if (prot & PROT_WRITE) flags |= PTE_WRITABLE;
	if (!(prot & PROT_EXEC)) flags |= PTE_NX;

	uint64_t cursor = vaddr;
	uint64_t remaining = length;
	while (remaining > 0) {
		void *phys = pmm_alloc();
		if (!phys) {
			/* Partial failure — unmap what we've done so far */
			return 0;
		}
		if (vmm_map_user((uint64_t *)t->pml4_phys, cursor,
				 (uint64_t)(uintptr_t)phys, flags) != 0) {
			pmm_free(phys);
			return 0;
		}
		cursor += 4096;
		remaining -= 4096;
	}

	return vaddr;
}

static int do_munmap(uint64_t addr, uint64_t length)
{
	struct cpu_context *ctx = cpu_get_ctx();
	struct thread *t = ctx->current;
	if (!t || !t->pml4_phys) return -1;

	/* Round up to page size */
	length = (length + 4095) & ~4095ULL;
	if (length == 0) return -1;
	addr &= ~4095ULL;

	/* Find the VMA */
	struct vma *v = vma_find(t, addr);
	if (!v || v->start != addr || v->end != addr + length)
		return -1;

	/* Unmap each page. Use phys_to_virt() inline to avoid any
	 * issues with the global kernel_pml4 variable. */
	for (uint64_t va = addr; va < addr + length; va += 4096) {
		uint64_t raw = vmm_get_phys((uint64_t)(uintptr_t)t->pml4_phys, va);
		uint64_t phys = raw & PTE_ADDR_MASK;
		vmm_unmap((uint64_t *)phys_to_virt((uint64_t)(uintptr_t)t->pml4_phys), va);
		if (phys)
			pmm_free((void *)(uintptr_t)phys);
	}

	/* Remove VMA from list */
	return vma_free(t, addr, length);
}

void syscall_handler(struct trap_frame *regs)
{
	asm volatile("sti");

	int64_t ret = -1;
	uint64_t syscall_num = regs->rax;

	switch (syscall_num) {
		case SYS_WRITE: // 1
			ret = sys_write((int)regs->rdi, (const char*)regs->rsi, (size_t)regs->rdx);
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
		case SYS_MMAP: { // 5: mmap(length=rdi, prot=rsi, flags=rdx)
			uint64_t length = regs->rdi;
			uint64_t prot   = regs->rsi;
			uint64_t map_flags = regs->rdx;
			ret = (int64_t)do_mmap(length, prot, map_flags);
			break;
		}
		case SYS_MUNMAP: { // 6: munmap(addr=rdi, length=rsi)
			uint64_t addr   = regs->rdi;
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
		L("[SYSCALL PREEMPT LOOP] CPU %d thr=%s no=%lu loops=%d",
				_c->id, _c->current->name, syscall_num, loops);
	}
}

/** arch_user_thread_entry_stub -- x86_64: construct iretq frame and jump to Ring 3 */
void arch_user_thread_entry_stub(struct thread *t)
{
	arch_local_irq_disable();

	tss_set_rsp0(cpu_get_ctx()->id, (uint64_t)t->kernel_stack);

	uint64_t stack_top = (uint64_t)t->kernel_stack_base + KERNEL_STACK_SIZE;
	stack_top &= ~0xfULL;

	L("[stub] going to user, entry=%p usp=%p\n", t->entry_point, t->user_stack);

	/* fill iretq frame -- no function calls after this */
	uint64_t *rsp = (uint64_t*)stack_top;
	rsp -= 5;
	rsp[0] = t->entry_point;   /* RIP  */
	rsp[1] = USER_CS_SEL;      /* CS   */
	rsp[2] = 0x202;            /* RFLAGS (IF=1) */
	rsp[3] = (uint64_t)t->user_stack;  /* RSP3 */
	rsp[4] = USER_SS_SEL;      /* SS3  */

	arch_enter_usermode(rsp);
}
