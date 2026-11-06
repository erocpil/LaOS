#ifndef __ATOMIC_H__
#define __ATOMIC_H__

/*
 * atomic.h — 基于 GCC 原子内建函数的操作接口
 */

#include <stdint.h>

/* 32 位原子计数器。 */
typedef struct {
	volatile int counter;
} atomic_t;

#define ATOMIC_INIT(i) {(i)}

/* 64 位原子计数器。 */
typedef struct {
	volatile int64_t counter; /* 或 int64_t */
} atomic64_t;

#define ATOMIC64_INIT(i) {(i)}

/* 内存序语义：
 *   __ATOMIC_RELAXED：仅保证该操作的原子性，不建立跨操作顺序；
 *   __ATOMIC_ACQUIRE：后续读写不能重排到该操作之前；
 *   __ATOMIC_RELEASE：先前读写不能重排到该操作之后；
 *   __ATOMIC_ACQ_REL：同时提供 acquire 与 release 语义；
 *   __ATOMIC_SEQ_CST：在所有顺序一致操作间建立单一全序。
 */

#define atomic_read(v)	 __atomic_load_n(&(v)->counter, __ATOMIC_RELAXED)
#define atomic_set(v, i) __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED)

#define atomic_inc(v) __atomic_add_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)
#define atomic_dec(v) __atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)

#define atomic_inc_return(v) __atomic_add_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)
#define atomic_dec_return(v) __atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)

#define atomic_inc_and_test(v) (atomic_inc_return(v) == 0)
#define atomic_dec_and_test(v) (atomic_dec_return(v) == 0)

#define atomic_add(i, v) __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_ACQ_REL)
#define atomic_sub(i, v) __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_ACQ_REL)

#if 1
#define atomic_cmpxchg(v, old, new)                                                                \
	__atomic_compare_exchange_n(                                                               \
		&(v)->counter, &(old), (new), 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)
#else
// x86 可使用 LOCK CMPXCHG 实现，并提供完整屏障语义。
// AArch64 实现需使用具备相应内存序的原子指令：
static inline int atomic_cmpxchg(atomic_t *v, int old, int new)
{
	// 使用 LDAXR/STLXR 对或 CAS 与 DMB 组合，满足 acquire-release 语义。
	int ret;
	unsigned long tmp;
	__asm__ volatile("1: ldaxr   %w0, [%2]       \n" // load-acquire
			 "   cmp     %w0, %w3        \n"
			 "   b.ne    2f              \n"
			 "   stlxr   %w1, %w4, [%2] \n" // store-release
			 "   cbnz    %w1, 1b         \n"
			 "2:                         \n"
		: "=&r"(ret), "=&r"(tmp)
		: "r"(&v->counter), "r"(old), "r"(new)
		: "memory", "cc");
	return ret;
}
#endif

/* 64 位操作接口。 */
#define atomic64_read(v)   __atomic_load_n(&(v)->counter, __ATOMIC_RELAXED)
#define atomic64_set(v, i) __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED)

#define atomic64_inc(v) __atomic_add_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)
#define atomic64_dec(v) __atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)

#define atomic64_inc_return(v) __atomic_add_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)
#define atomic64_dec_return(v) __atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_ACQ_REL)

#define atomic64_inc_and_test(v) (atomic64_inc_return(v) == 0)
#define atomic64_dec_and_test(v) (atomic64_dec_return(v) == 0)

#define atomic64_add(i, v) __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_ACQ_REL)
#define atomic64_sub(i, v) __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_ACQ_REL)

#define atomic64_cmpxchg(v, old, new)                                                              \
	__atomic_compare_exchange_n(                                                               \
		&(v)->counter, &(old), (new), 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)

#endif
