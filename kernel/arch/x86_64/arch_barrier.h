#ifndef __ARCH_BARRIER_H__
#define __ARCH_BARRIER_H__

/* -------------------------------------------------------------
 * arch_barrier.h - x86_64 内存屏障与 CPU 暂停原语
 *
 * 设计原则：
 *   - 命名沿用 Linux 内核约定(smp_mb / smp_wmb / smp_rmb /
 *     smp_mb__after_atomic / cpu_relax)，便于阅读对应文档与代码
 *   - 全部 __always_inline，零调用开销
 *   - 注释保留 x86 TSO 内存模型的取舍依据，避免误用
 *
 * 与编译器屏障的区别：
 *   - 编译器屏障  asm volatile("" ::: "memory")   仅阻止编译器重排
 *   - 硬件屏障    asm volatile("mfence" ::: "memory") 阻止 CPU 重排
 *   smp_mb() 同时具备两者
 * ------------------------------------------------------------- */

#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif

/* cpu_relax - 自旋循环中降低功耗与超线程资源占用
 * x86 PAUSE 指令告诉 CPU 这是自旋，缩短退出 spin-loop 时的内存违例延迟。 */
static __always_inline void cpu_relax(void)
{
	__asm__ volatile("pause" ::: "memory");
}

/* -------------------------------------------------------------
 * smp_mb() - Full memory barrier
 *
 * x86_64 内存模型 (TSO: Total Store Order) 天然保证：
 *   Store->Store  不重排  [OK]
 *   Load->Load    不重排  [OK]
 *   Load->Store   不重排  [OK]
 *   Store->Load   可重排  [X]  <- 唯一需要硬件屏障的方向
 *
 * MFENCE 是 x86 上唯一能阻止 Store->Load 重排的指令。
 * LFENCE/SFENCE 在 TSO 下对普通 WB 内存不提供完整语义，不能替代。
 * ------------------------------------------------------------- */
static __always_inline void smp_mb(void)
{
	asm volatile("mfence" ::: "memory");
}

/* smp_wmb - Store barrier
 * x86 TSO 下 store 天然有序，sfence 仅对 NT-store (movntq/movntdq 等 WC 写)
 * 场景必需。普通 WB 内存写屏障用编译器屏障即可。 */
static __always_inline void smp_wmb(void)
{
	asm volatile("sfence" ::: "memory");
}

/* smp_rmb - Load barrier
 * x86 TSO 下 load 天然有序，lfence 主要用于阻止 speculative load
 * (Spectre 缓解).普通 SMP 同步不需要。 */
static __always_inline void smp_rmb(void)
{
	asm volatile("lfence" ::: "memory");
}

/* -------------------------------------------------------------
 * smp_mb__after_atomic()
 *
 * 用途：在 atomic_cmpxchg / LOCK 前缀的原子 RMW 之后声明 acquire 或
 *       full barrier 语义.x86 上 LOCK 前缀指令已是 full barrier,
 *       本接口退化为编译器屏障。
 *
 * 注意:atomic_set() 通常实现为普通 MOV (无 LOCK 前缀)，不含 barrier
 *       语义。在 atomic_set 后若需 release 语义，应调用 smp_mb()
 *       (完整硬件屏障)，而非本函数。
 *
 * 使用规范：
 *   场景1: atomic_cmpxchg 后 (mutex_lock 获取锁成功)
 *          cmpxchg 带 LOCK 前缀 -> 已是 full barrier
 *          -> smp_mb__after_atomic() 仅防编译器重排，足够
 *
 *   场景2: atomic_set 后 (mutex_unlock 释放锁)
 *          MOV 指令无 barrier -> 必须用 smp_mb()
 *
 *          错误用法：
 *              atomic_set(&m->locked, 0);
 *              smp_mb__after_atomic();     <- 不够！MOV 无硬件 barrier
 *
 *          正确用法：
 *              smp_mb();
 *              atomic_set(&m->locked, 0)；  <- 或用 xchg 代替 MOV
 *
 *   场景3: 用 xchg 替代 atomic_set 实现 release store (推荐):
 *              static __always_inline void atomic_set_release(atomic_t *v, int i)
 *              {
 *                  // xchg 带隐含 LOCK，是 full barrier
 *                  asm volatile("xchgl %0, %1"
 *                               : "+r"(i), "+m"(v->counter)
 *                               :: "memory");
 *              }
 * ------------------------------------------------------------- */
static __always_inline void smp_mb__after_atomic(void)
{
	asm volatile("" ::: "memory");
}

#endif
