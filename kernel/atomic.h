#ifndef __ATOMIC_H__
#define __ATOMIC_H__

/*
 * atomic.h - 原子操作宏(基于 GCC builtin)
 */

#include <stdint.h>

/* atomic_t (32位) */
typedef struct {
	volatile int counter;
} atomic_t;

#define ATOMIC_INIT(i) { (i) }

/* atomic64_t (64位) */
typedef struct {
	volatile int64_t counter;   /* 或 int64_t */
} atomic64_t;

#define ATOMIC64_INIT(i) { (i) }

/* 通用原子操作宏 */
/* 内存序(memory order)说明：
 *   __ATOMIC_RELAXED: 最宽松，无顺序保证(性能最高)
 *   __ATOMIC_ACQUIRE: 读取时 acquire(后续读写不能重排到前面)
 *   __ATOMIC_RELEASE: 写入时 release(前面读写不能重排到后面)
 *   __ATOMIC_ACQ_REL: acquire + release
 *   __ATOMIC_SEQ_CST: 最强顺序(默认，类似老的 __sync)
 */

#define atomic_read(v)         __atomic_load_n(&(v)->counter, __ATOMIC_RELAXED)
#define atomic_set(v, i)       __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED)

#define atomic_inc(v)          __atomic_add_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)
#define atomic_dec(v)          __atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)

#define atomic_inc_return(v)   __atomic_add_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)
#define atomic_dec_return(v)   __atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)

#define atomic_inc_and_test(v) (atomic_inc_return(v) == 0)
#define atomic_dec_and_test(v) (atomic_dec_return(v) == 0)

#define atomic_add(i, v)       __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_ACQ_REL)
#define atomic_sub(i, v)       __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_ACQ_REL)

#if 1
#define atomic_cmpxchg(v, old, new) \
	__atomic_compare_exchange_n(&(v)->counter, &(old), (new), 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)
#else
// x86: 直接映射到 LOCK CMPXCHG 指令，天然 full barrier
// ARM64 实现要点：
static inline int atomic_cmpxchg(atomic_t *v, int old, int new)
{
	// 必须用 LDAXR/STLXR 对(load-acquire / store-release)
	// 或者 CAS + DMB 组合，提供 acquire-release 语义
	int ret;
	unsigned long tmp;
	__asm__ volatile(
			"1: ldaxr   %w0, [%2]       \n"  // load-acquire
			"   cmp     %w0, %w3        \n"
			"   b.ne    2f              \n"
			"   stlxr   %w1, %w4, [%2] \n"  // store-release
			"   cbnz    %w1, 1b         \n"
			"2:                         \n"
			: "=&r"(ret), "=&r"(tmp)
			: "r"(&v->counter), "r"(old), "r"(new)
			: "memory", "cc");
	return ret;
}
#endif

/* 64位版本 */
#define atomic64_read(v)         __atomic_load_n(&(v)->counter, __ATOMIC_RELAXED)
#define atomic64_set(v, i)       __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED)

#define atomic64_inc(v)          __atomic_add_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)
#define atomic64_dec(v)          __atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)

#define atomic64_inc_return(v)   __atomic_add_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)
#define atomic64_dec_return(v)   __atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)

#define atomic64_inc_and_test(v) (atomic64_inc_return(v) == 0)
#define atomic64_dec_and_test(v) (atomic64_dec_return(v) == 0)

#define atomic64_add(i, v)       __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_ACQ_REL)
#define atomic64_sub(i, v)       __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_ACQ_REL)

#define atomic64_cmpxchg(v, old, new) \
	__atomic_compare_exchange_n(&(v)->counter, &(old), (new), 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)

#endif
