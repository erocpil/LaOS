#ifndef __THREAD_H__
#define __THREAD_H__

/*
 * thread.h - 内核线程类型定义
 */

#include <stdint.h>

#include "list.h"
#include "atomic.h"

/* Forward declaration — vma.h includes thread.h */
struct vma;

// 分配一个足够大的内核栈。ARM64 异常入口会在当前线程内核栈上保存
// 完整 trap frame；保持 128KB 与通用 STACK_SIZE 一致，避免 EL0/SMP
// 路径在异常嵌套或调度切换后贴近未映射边界。
#define KERNEL_STACK_PAGES 32
#define KERNEL_STACK_SIZE  (PAGE_SIZE * KERNEL_STACK_PAGES)

/* Common size for architecture-owned per-CPU interrupt stacks.
 * ARM64 uses this model in its exception entry.  x86_64 regular IRQ/IPI
 * entry does not switch stacks yet; its scheduler needs the suspended
 * interrupt continuation to remain owned by the interrupted thread. */
#define INT_STACK_PAGES 2
#define INT_STACK_SIZE   (PAGE_SIZE * INT_STACK_PAGES)

// 线程状态
typedef enum {
	// 就绪：可以被调度
	THREAD_READY = 0,
	// 运行：正在占用 CPU
	THREAD_RUNNING,
	// 阻塞：正在等待某个资源(如 Mutex)，不参与调度
	THREAD_BLOCKED,
	// 休眠
	THREAD_SLEEPING,
	// 退出
	THREAD_ZOMBIE,
	// 退出：已结束
	THREAD_EXITED,
	// 计数
	THREAD_MAX,
} thread_status_t;

extern char *THREAD_STATUS_STR[];
extern struct thread *g_current_thread;

/* O(1) 调度器优先级 (P4-6)
 * 0 = 最高，SCHED_PRIO_COUNT-1 = 最低。默认放在中间。 */
#define SCHED_PRIO_COUNT      64
#define SCHED_PRIO_BITMAP_SZ  ((SCHED_PRIO_COUNT + 63) / 64)
#define SCHED_DEFAULT_PRIO    32
#define SCHED_PRIO_NONE       SCHED_PRIO_COUNT

#define THREAD_NAME_MAX 31
// 线程控制块 Thread Control Block
struct thread {
	// 保存的栈顶指针 (必须放在结构体第一个，方便汇编访问)
	// 切换时保存的上下文指针
	uint64_t rsp;

	// 偏移 8，方便通过 gs:8 拿到自己
	struct thread *self;

	// 线程 ID
	int64_t id;

	// 内存管理
	// 该线程对应的 PML4 页表物理地址 (内核线程指向 kernel_pml4)
	uint64_t *pml4_phys;

	// 标志位
	// -1 表示可以跑在任何 CPU，0-3 表示绑定特定核
	int target_cpu;
	int is_idle;
	uint64_t ticks;
	// 是否是用户态线程
	int is_user;
	int exit_code;
	// 用户态入口地址
	uint64_t entry_point;

	// task.conf 加载的模块参数（argc/argv），0 表示非模块入口
	int entry_argc;
	char **entry_argv;

	// 状态追踪
	volatile thread_status_t status;

	// 栈空间管理
	void *user_stack; // 用户栈基地址 (只有用户进程需要)
	void *kernel_stack; // 内核栈基地址 (kmalloc 分配的)
	void *kernel_stack_base;

	// ELF 副本：线程退出时由 thread_destroy 释放
	void *elf_load_addr;
	// ELF 副本大小（module_apply_kv_params 需要定位 section header）
	uint64_t elf_size;
	// 用户栈物理页：线程退出时由 thread_destroy 调用 pmm_free
	uint64_t user_stack_phys;

	// 应该被唤醒的时间点 (以系统的 jiffies 或 ms 为单位)
	uint64_t wakeup_ticks;
	uint64_t sleep_times;
	uint64_t run_tsc;
	uint64_t last_tsc;

	uint64_t last_snapshot_tsc;
	uint64_t last_cpu_usage;

	// 被抢占次数
	uint32_t preempts;

	int rcu_blocked;
	struct list_node rcu_blocked_node;
	// RCU 读临界区嵌套深度.per-thread 字段。
	// 仅本 CPU(即 current==该 thread 时)读写，无跨 CPU 并发：
	// reader 在持有 nesting>0 的状态下被切走时，本字段随线程走，
	// 切回时延续其嵌套上下文.memset 0 初始化天然满足初始无嵌套。
	int rcu_nesting;

	// 绑定的虚拟终端 ID (例如 0， 1， 2)
	int tty_id;

	void *data;

	/*
	 * Fixed scheduler priority: 0 is highest.  priority is the effective
	 * value used by the runqueue; base_priority is the caller-selected
	 * value restored after all mutex donations disappear.
	 */
	uint8_t priority;
	uint8_t base_priority;
	uint16_t pi_donations[SCHED_PRIO_COUNT];
	atomic_t pi_lock;

	/* Per-thread VMA list (mmap/munmap tracking) */
	struct vma *vma_list;

	char name[THREAD_NAME_MAX + 1];

	struct list_node node;
	struct list_node wait_node;

	/*
	 * FPU/SSE 寄存器保存区(512 字节，16 字节对齐).
	 *
	 * fxsave64 / fxrstor64 指令要求目标地址 16 字节对齐。
	 * __attribute__((aligned(16))) 保证编译器在字段前插入足够 padding，
	 * 使 fpu_state 在 struct thread 内的偏移量为 16 的倍数。
	 *
	 * 零初始化即 x86 定义的 FPU reset state:
	 *   FCW = 0x037F(64-bit double precision, all exceptions masked)
	 *   MXCSR = 0x1F80(all SSE exceptions masked, flush-to-zero off)
	 *   所有 ST/MM/XMM 寄存器为 +0.0，tag word = 0xFFFF(empty)
	 *
	 * 如果未来需要 lazy FPU switching(仅在 FPU 被实际使用时才 save)，
	 * 可在 struct thread 增 has_fpu 标志，配合 #NM (Device Not Available)
	 * 异常 handler 实现按需 save/restore.当前为了保证正确性，采用 eager
	 * switching:每次上下文切换都无条件 save/restore.
	 *
	 * 大小:fxsave/fxrstor 固定写入 512 字节;xsave/xrstor(AVX 需要)
	 * 可用 CPUID 查询实际大小，但当前内核禁用 SSE/AVX(-mno-sse),
	 * 512 字节已覆盖未来启用 SSE 后的 XMM 寄存器空间。
	 *
	 * ARM64 需要 528 字节(32×Q 寄存器 512 + FPSR/FPCR 8),
	 * 槽位扩至 576 以容纳两架构。x86_64 仅用前 512 字节。
	 */
	__attribute__((aligned(16))) uint8_t fpu_state[576];
} __attribute__((__aligned__(sizeof(char))));

// 这里的参数顺序要和汇编里的寄存器顺序(RDI， RSI)对应
extern void switch_to(volatile struct thread *old_thread, struct thread *new_thread);
extern void ret_from_fork(void);

/* FPU reset-state template (576 bytes slot, x86_64 uses first 512).
 * Generated once in cpu_enable_fpu() via fninit+ldmxcsr+fxsave64.
 * All-zeros is NOT a valid FXSAVE image — FCW and MXCSR differ.
 * Declared in arch/x86_64/thread_arch.h; aarch64 no-ops. */

#define DUMP_THREAD(t) \
	do { \
		kprintf("[%s %d]\n" \
				"thread   %p\n" \
				"  id     %d\n" \
				"  name   %s\n" \
				"  user   %d\n" \
				"  cpu    %d\n", \
				__func__, __LINE__, \
				t, t->id, t->name, t->is_user, t->target_cpu); \
	} while (0)

/* 通过 cpu_context 取当前线程:GS 基址指向 per-CPU cpu_context,
 * ctx->current 由 __schedule 在切线程时更新。 */
#define get_current() ({ \
		cpu_get_ctx()->current; \
		})

// 简单的调度器接口
void thread_init_main(void);
struct thread *thread_create(void (*entry)(void*), void *data);
struct thread *thread_create_on(void (*entry)(void*), void *data, int cpu);
struct thread *thread_create_idle(void (*entry)(void*), void *data);
struct thread *thread_create_common(void (*entry)(void*), void *data);
void thread_entry_point(void *data, void *entry_func);
void thread_destroy(struct thread *t);
void user_thread_entry_stub(struct thread *t);
uint64_t thread_get_current_id(void);
void idle_task_function(void *arg);
int thread_set_name(struct thread *t, const char *name);
void thread_set_target_cpu(struct thread *t, int32_t cpu_id);
void thread_priority_init(struct thread *t);
int thread_set_priority(struct thread *t, int priority);
void thread_priority_update_donation(struct thread *t,
		int old_priority, int new_priority);
struct thread *create_elf_process(uint8_t *elf_raw, int elf_size, int argc, void *argv);
void thread_setup_user_frame(struct thread *t, void *stack_base,
	void (*entry_func)(void*), uint64_t user_arg);

/** arch_user_thread_entry_stub — 架构相关的用户态入口跳板
 *  x86_64: 构造 iretq 帧并执行 swapgs+iretq
 *  aarch64: 切换 TTBR0 并 eret 到 EL0 */
void arch_user_thread_entry_stub(struct thread *t);

/* 内核线程 ELF 加载统一入口。
 * flags 决定 ELF 类型：
 *   KTHREAD_ELF_EXEC - 已链接 ET_EXEC，直接载入并跳到 e_entry
 *   KTHREAD_ELF_REL  - 可重定位 ET_REL(.mo 模块)，需 BSS 独立分配和重定位
 */
#define KTHREAD_ELF_EXEC 0
#define KTHREAD_ELF_REL  1
struct thread *kthread_load_elf(uint8_t *elf_raw, int elf_size, int flags,
		const char *name, void *data);
void thread_exit(int exit_code);

/* kv 参数应用：匹配模块的 __laos_params 段，将 task.conf 的 key=value 对写入模块变量 */
void module_apply_kv_params(struct thread *th, int kv_count,
		char *kv_keys[], char *kv_values[]);
void thread_set_status(struct thread *t, thread_status_t s);

/* PID 分配宏：内核线程取负值，用户线程取正值。
 * 全局计数器 g_thread_kernel_count / g_thread_user_count 定义在 thread.c. */
extern atomic64_t g_thread_kernel_count;
extern atomic64_t g_thread_user_count;
#define THREAD_SET_KERNEL_PID() ({ \
		-atomic64_inc_return(&g_thread_kernel_count); \
		})
#define THREAD_SET_USER_PID() ({ \
		atomic64_inc_return(&g_thread_user_count); \
		})

#define thread_set_status(t, s) \
	do { \
		__typeof__(t) _t = (t); \
		__atomic_store_n(&_t->status, s, __ATOMIC_RELEASE); \
	} while (0)

#define thread_get_status(t) ({ \
		__typeof__(t) _t = (t); \
		__atomic_load_n(&_t->status, __ATOMIC_ACQUIRE); \
		})

#endif
