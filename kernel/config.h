#ifndef __CONFIG_H__
#define __CONFIG_H__

/**
 * LaOS 编译期特性开关
 *
 * 约定：所有特性开关用 `#if CONFIG_XXX` 而非 `#ifdef CONFIG_XXX`，宏永远定义为
 *       0 或 1，便于在表达式上下文使用，避免 typo 意外当作"未定义=关闭"。
 */

/**
 * RCU 支持开关。
 *
 *   CONFIG_RCU = 1: 启用 preemptible RCU(方案 B)
 *     - timer.c 在 tick ISR 调用 rcu_check_quiescent_state()
 *     - sched.c 在 __schedule 路径调用 rcu_note_context_switch()
 *     - rcu_read_lock / rcu_read_unlock 走真实 nesting + blocked_tasks 路径
 *
 *   CONFIG_RCU = 0: 关闭 RCU。集成钩子和 reader API 退化为 no-op，
 *     rcu_stress 不注册。用于隔离调试：把 RCU 路径从问题域切掉看
 *     现象是否仍重现。
 *
 * 调用点本身不需要 #if 包裹：靠头部条件编译让目标符号在关闭时变 stub。
 */
#ifndef CONFIG_RCU
#define CONFIG_RCU 1
#endif

/**
 * CONFIG_RCU_DEBUG - RCU 调试日志开关
 *
 *   CONFIG_RCU_DEBUG = 1: rcu_note_context_switch 和 rcu_read_unlock
 *     刷详细链表快照到串口，仅开发期诊断 blocked_tasks 链表状态。
 *
 *   CONFIG_RCU_DEBUG = 0: 静默运行，无串口开销。
 */
#ifndef CONFIG_RCU_DEBUG
#define CONFIG_RCU_DEBUG 0
#endif

/**
 * PMM 自测开关。
 *
 *   CONFIG_PMM_SELFTEST = 1: pmm_init 末尾跑 pmm_test_recycling + pmm_test_lowmem_guard，串口刷一屏自检日志。
 *     自测会人为 BITMAP_CLEAR(100) 触碰 bitmap 内部状态(white-box)，仅开发期使用。
 *
 *   CONFIG_PMM_SELFTEST = 0: 自测函数与调用点全部条件编译消除，boot 期无开销，无串口噪声。
 */
#ifndef CONFIG_PMM_SELFTEST
#define CONFIG_PMM_SELFTEST 0
#endif

/**
 * CONFIG_IST_TEST - 启用 IST 中断栈隔离自检测试
 *
 *   CONFIG_IST_TEST = 1: boot 后期启动一个 kthread，以深度递归耗尽自己的 kernel stack，
 *     然后进入无穷循环等下一次时钟中断。中断触发后 CPU 检测到栈越界压不下去，硬件抛
 *     #DF，走 IST_SLOT_DF 独立栈进入 double-fault handler，安全 panic。
 *     若 IST 未生效，会 triple-fault 重启(无输出)。
 *
 *   CONFIG_IST_TEST = 0: 测试线程不启动，无运行开销。
 */
#ifndef CONFIG_IST_TEST
#define CONFIG_IST_TEST 0
#endif

/**
 * CONFIG_TASK_PREEMPT_TEST - 抢占测试线程开关
 *
 *   CONFIG_TASK_PREEMPT_TEST = 1: task_run_cpu 启动一个 busy-wait kthread,
 *     每轮持锁，置 need_resched，解锁->preempt_enable 兑现抢占，
 *     验证调度器抢占路径正常。
 *
 *   CONFIG_TASK_PREEMPT_TEST = 0: 不启动测试线程。
 */
#ifndef CONFIG_TASK_PREEMPT_TEST
#define CONFIG_TASK_PREEMPT_TEST 0
#endif

/**
 * CONFIG_TTY_TEST - TTY 多路复用单测开关
 *
 *   CONFIG_TTY_TEST = 1: boot 后期启动 tty_test kthread，遍历 tty_ready() 的 6
 *   种输入组合(TTY 0 通配 / TTY 6-9 bitmask 匹配 / 未初始化拒绝)，验证结果与预期一致。
 *
 *   CONFIG_TTY_TEST = 0: 测试线程不启动，无运行开销。
 */
#ifndef CONFIG_TTY_TEST
#define CONFIG_TTY_TEST 0
#endif

/**
 * CONFIG_TIMER_DEBUG_PRINT - PIT 滴答屏幕打印开关
 *
 *   CONFIG_TIMER_DEBUG_PRINT = 1: timer_handler 在屏幕左下角打印各 CPU
 *     uptime 秒数(调试/演示用途，每 tick 写 framebuffer 有性能开销)。
 *
 *   CONFIG_TIMER_DEBUG_PRINT = 0: 不打印 uptime，无额外开销。
 */
#ifndef CONFIG_TIMER_DEBUG_PRINT
#define CONFIG_TIMER_DEBUG_PRINT 0
#endif

#ifndef CONFIG_DEBUG
#define CONFIG_DEBUG 0
#endif

/**
 * CONFIG_VMM_TEST - VMM 自毁测试开关
 *
 *   CONFIG_VMM_TEST = 1: vmm_test() 最后一步在 unmap 后故意写已释放页，
 *     触发 Page Fault 验证缺页异常机制。仅开发期使用。
 *
 *   CONFIG_VMM_TEST = 0: 跳过自毁步骤，测试在 unmap 后正常返回。
 */
#ifndef CONFIG_VMM_TEST
#define CONFIG_VMM_TEST 0
#endif

/**
 * CONFIG_MUTEX_HANDOFF - mutex 锁移交策略
 *
 *   CONFIG_MUTEX_HANDOFF = 1: handoff 语义。unlock 时直接将 owner 移交给
 *     队首等待者(locked 保持 1)，新来者必须排队。防止等待者饥饿，
 *     适合对公平性有要求的场景。
 *
 *   CONFIG_MUTEX_HANDOFF = 0: raw 语义。unlock 时先释放锁再唤醒等待者，
 *     新来者可与被唤醒线程竞争(barging)。实现简单，吞吐量更高，但等待者可能饥饿。
 *
 *   详见 docs/mutex-design.md。
 */
#ifndef CONFIG_MUTEX_HANDOFF
#define CONFIG_MUTEX_HANDOFF 0
#endif

/*
 * Legacy unbounded mutex stress workers.  Keep them opt-in: starting one on
 * every CPU during normal boot permanently occupies the default-priority
 * runqueue and makes fixed-priority starvation behavior unavoidable.
 */
#ifndef CONFIG_MUTEX_STRESS
#define CONFIG_MUTEX_STRESS 0
#endif

/**
 * CONFIG_PF_TEST - 切换异常测试类型(需 CONFIG_EXCEPTION_TEST=1)
 *
 *   CONFIG_PF_TEST = 0: 触发除零异常(#DE)，演示 GPR dump / CR0+CR4 解码 / 回溯
 *   CONFIG_PF_TEST = 1: 触发缺页异常(#PF)，演示页表遍历 dump + CR4 解码
 *
 *   #PF 地址固定为 0x1000(未映射)，页表 walk 会逐级显示 PML4->PDPT->PD->PT
 *   的条目值和标志位。
 */
#ifndef CONFIG_PF_TEST
#define CONFIG_PF_TEST 0
#endif

/**
 * CONFIG_EXCEPTION_TEST - 异常处理器自测开关
 *
 *   CONFIG_EXCEPTION_TEST = 1: kmain 末尾触发除零异常(#DE)，验证
 *     exception_handler 的 GPR dump / CR0+RFLAGS 解码 / 指令字节 /
 *     符号解析 / 栈回溯等全部输出项，然后 halt。
 *
 *   CONFIG_EXCEPTION_TEST = 0: 测试代码被预处理器删除，零开销。
 *
 *   测试调用链(三层 noinline，均 EXPORT_SYMBOL):
 *     kmain -> exception_test_lv1 -> exception_test_lv2 -> exception_test_trigger -> idiv
 */
#ifndef CONFIG_EXCEPTION_TEST
#define CONFIG_EXCEPTION_TEST 0
#endif

#endif
