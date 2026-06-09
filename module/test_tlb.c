/*
 * test_tlb.mo — SMP TLB shootdown remap visibility selftest (dual-arch)
 *
 * Exports selftest_init() which is called synchronously by the kernel
 * selftest loader before schedule().  Registers as "smp_tlb_remap".
 *
 * Controlled by task.conf directive:
 *   @test smp_tlb_remap module=test_tlb.mo rounds=N
 *
 * Builds for both x86_64 and aarch64; uses kernel IPI broadcast
 * (IPI_VECTOR_TLB) + vmm_remap() for cross-CPU TLB-invalidation
 * verification.
 */
#include "selftest.h"
#include "ipi.h"
#include "vmm.h"
#include "module_alloc.h"
#include "pmm.h"
#include "hhdm.h"
#include "printf.h"
#include "cpu.h"
#include "export.h"
#include "string.h"

/* ── Test state ─────────────────────────────────────────── */
#define TLB_STRESS_VALUE_A 0x4c614f53544c4231ULL /* "LaOSTLB1" */
#define TLB_STRESS_VALUE_B 0x4c614f53544c4232ULL /* "LaOSTLB2" */

static volatile uint32_t s_rounds = 1;
static volatile uint32_t s_current_round;
static volatile uint32_t s_stage;
static volatile uint64_t s_expected_value;
static volatile uint64_t s_phase_count;
static volatile bool s_done;
static uintptr_t s_va;
static void *s_page_a;
static void *s_page_b;
static uint64_t s_expected_aps;

/* ── IPI callback (runs on every AP) ────────────────────── */
static void tlb_stress_ipi_cb(void)
{
	uint32_t stage = __atomic_load_n(&s_stage, __ATOMIC_ACQUIRE);
	if (stage != 1 && stage != 2)
		return;

	uintptr_t va = __atomic_load_n(&s_va, __ATOMIC_ACQUIRE);
	uint64_t expected = __atomic_load_n(&s_expected_value, __ATOMIC_ACQUIRE);
	if (va && (*(volatile uint64_t *)va) == expected)
		module_atomic_add_fetch((volatile uint64_t *)&s_phase_count, 1);
}

/* ── selftest API ───────────────────────────────────────── */
static void tlb_test_configure(const char *key, const char *value)
{
	if (strcmp(key, "rounds") == 0) {
		uint32_t r = 0;
		for (const char *c = value; *c >= '0' && *c <= '9'; c++)
			r = r * 10 + (uint32_t)(*c - '0');
		if (r == 0) r = 1;
		if (r > 64) r = 64;
		__atomic_store_n(&s_rounds, r, __ATOMIC_RELEASE);
	}
}

static void tlb_test_start(void)
{
	/* Register IPI callback so APs invoke our visibility check */
	ipi_tlb_set_callback(tlb_stress_ipi_cb);

	/* Read AP count: online includes BSP, so expected = online - 1 */
	uint64_t total = __atomic_load_n(&online, __ATOMIC_ACQUIRE);
	s_expected_aps = (total > 0) ? total - 1 : 0;
	kprintf("[tlb] start: online=%llu expected_aps=%llu rounds=%u\n",
		total, s_expected_aps, s_rounds);
}

static void tlb_test_tick(void)
{
	uint64_t expected = s_expected_aps;
	if (!expected)
		return;

	uint64_t online_now = __atomic_load_n(&online, __ATOMIC_ACQUIRE);
	if (online_now < expected + 1)  /* +1 = BSP */
		return;

	uint32_t stage = __atomic_load_n(&s_stage, __ATOMIC_ACQUIRE);

	if (stage == 0) {
		uintptr_t base = module_region_base();
		uint32_t rounds = __atomic_load_n(&s_rounds, __ATOMIC_ACQUIRE);

		if (!base || base < PAGE_SIZE) {
			kprintf("[tlb] skipped: no module region\n");
			__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
			return;
		}
		s_page_a = pmm_alloc();
		s_page_b = pmm_alloc();
		if (!s_page_a || !s_page_b) {
			kprintf("[tlb] skipped: pmm_alloc\n");
			if (s_page_a) pmm_free(s_page_a);
			if (s_page_b) pmm_free(s_page_b);
			__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
			return;
		}
		*(volatile uint64_t *)phys_to_virt((uint64_t)s_page_a) = TLB_STRESS_VALUE_A;
		*(volatile uint64_t *)phys_to_virt((uint64_t)s_page_b) = TLB_STRESS_VALUE_B;

		s_va = base - PAGE_SIZE;
		if (vmm_map_global(s_va, (uint64_t)s_page_a,
				PTE_PRESENT | PTE_WRITABLE) != 0) {
			kprintf("[tlb] skipped: map failed\n");
			pmm_free(s_page_a); pmm_free(s_page_b);
			s_page_a = NULL; s_page_b = NULL; s_va = 0;
			__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
			return;
		}

		__atomic_store_n(&s_expected_value, TLB_STRESS_VALUE_A, __ATOMIC_RELEASE);
		__atomic_store_n(&s_phase_count, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&s_stage, 1, __ATOMIC_RELEASE);
		kprintf("[tlb] TLB remap stress: rounds=%u\n", rounds);
		ipi_broadcast(IPI_VECTOR_TLB);
		return;
	}

	if (stage == 1) {
		uint64_t got = __atomic_load_n(&s_phase_count, __ATOMIC_ACQUIRE);
		if (got < expected)
			return;
		__atomic_store_n(&s_current_round, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&s_stage, 2, __ATOMIC_RELEASE);
	}

	if (stage == 2) {
		uint32_t rounds = __atomic_load_n(&s_rounds, __ATOMIC_ACQUIRE);
		uint32_t current = __atomic_load_n(&s_current_round, __ATOMIC_ACQUIRE);
		uint64_t got = __atomic_load_n(&s_phase_count, __ATOMIC_ACQUIRE);

		if (current > 0 && got < expected)
			return;

		if (current >= rounds) {
			kprintf("[tlb] TLB remap visible: %llu/%llu rounds=%u\n",
				expected, expected, rounds);
			__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
			/* Zero s_va BEFORE vmm_unmap/free — vmm_unmap
			 * broadcasts IPI which triggers the callback;
			 * the callback reads s_va and must see NULL. */
			uintptr_t old_va = s_va;
			s_va = 0;
			if (old_va) vmm_unmap(kernel_pml4, old_va);
			if (s_page_a) pmm_free(s_page_a);
			if (s_page_b) pmm_free(s_page_b);
			s_page_a = NULL;
			s_page_b = NULL;
			return;
		}

		void *target = (current & 1) ? s_page_a : s_page_b;
		uint64_t value = (current & 1) ? TLB_STRESS_VALUE_A : TLB_STRESS_VALUE_B;
		__atomic_store_n(&s_expected_value, value, __ATOMIC_RELEASE);
		__atomic_store_n(&s_phase_count, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&s_current_round, current + 1, __ATOMIC_RELEASE);
		if (vmm_remap(kernel_pml4, s_va, (uint64_t)target,
				PTE_PRESENT | PTE_WRITABLE) != 0) {
			kprintf("[tlb] TLB remap failed: vmm_remap\n");
			__atomic_store_n(&s_done, true, __ATOMIC_RELEASE);
		}
	}
}

static bool tlb_test_done(void)
{
	return __atomic_load_n(&s_done, __ATOMIC_ACQUIRE);
}

static const struct selftest smp_tlb_remap_test = {
	.name      = "smp_tlb_remap",
	.configure = tlb_test_configure,
	.start     = tlb_test_start,
	.tick      = tlb_test_tick,
	.done      = tlb_test_done,
};

/* Called synchronously by kernel selftest loader */
int selftest_init(void)
{
	kprintf("[tlb] selftest_init\n");
	return selftest_register(&smp_tlb_remap_test);
}
EXPORT_SYMBOL(selftest_init);
