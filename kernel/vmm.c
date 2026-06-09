/*
 * vmm.c - 4 级页表管理(VMM)
 *
 * 每任务独立 PML4.提供 vmm_map/vmm_unmap/vmm_destroy_level，
 * 支持 NX/US 标记，PML4 递归映射。
 */

#include <stdbool.h>
#include <limine.h>

#include "config.h"
#include "pmm.h"
#include "vmm.h"
#include "ipi.h"
#include "string.h"
#include "printf.h"
#include "export.h"
#include "debug.h"
#include "hhdm.h"
#include "arch_tlb.h"

int vmm_is_mapped(uint64_t pml4_phys, uint64_t vaddr)
{
	// 1. 分解虚拟地址，获取每一级页表的索引
	uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
	uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
	uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
	uint64_t pt_idx = (vaddr >> 12) & 0x1FF;

	// 2. 检查 PML4E (Page Map Level 4 Entry)
	uint64_t *pml4 = (uint64_t*)phys_to_virt((uint64_t)pml4_phys);
	if (!(pml4[pml4_idx] & 0x1)) {
		return false; // Present 位为 0
	}

	// 3. 检查 PDPTE (Page Directory Pointer Table Entry)
	uint64_t *pdpt = (uint64_t*)phys_to_virt((uint64_t)pml4[pml4_idx] & ~0xFFF);
	if (!(pdpt[pdpt_idx] & 0x1)) {
		return false;
	}
	// 如果是 1GB 大页 (PS位为 1)，则认为已映射
	if (PTE_IS_LEAF(pdpt[pdpt_idx])) {
		return true;
	}

	// 4. 检查 PDE (Page Directory Entry)
	uint64_t *pd = (uint64_t*)phys_to_virt((uint64_t)pdpt[pdpt_idx] & ~0xFFF);
	if (!(pd[pd_idx] & 0x1)) {
		return false;
	}
	// 如果是 2MB 大页 (PS位为 1)，则认为已映射
	if (PTE_IS_LEAF(pd[pd_idx])) {
		return true;
	}

	// 5. 检查 PTE (Page Table Entry)
	uint64_t *pt = (uint64_t*)phys_to_virt((uint64_t)pd[pd_idx] & ~0xFFF);
	if (!(pt[pt_idx] & 0x1)) {
		return false;
	}

	return true;
}

uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t vaddr)
{
	// 调试：如果 pml4_phys 看起来像个虚拟地址(高位为1)，报警
	if (pml4_phys > 0x00007FFFFFFFFFFF) {
		panic("ERROR: vmm_get_phys received a VIRTUAL address: %p\n", pml4_phys);
	}

	uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
	uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
	uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
	uint64_t pt_idx = (vaddr >> 12) & 0x1FF;

	uint64_t *pml4 = (uint64_t*)phys_to_virt((uint64_t)pml4_phys);
	if (!(pml4[pml4_idx] & 0x1)) {
		return 0;
	}

	uint64_t *pdpt = (uint64_t*)phys_to_virt((uint64_t)pml4[pml4_idx] & ~0xFFF);
	if (!(pdpt[pdpt_idx] & 0x1)) {
		return 0;
	}
	if (PTE_IS_LEAF(pdpt[pdpt_idx])) {
		return (pdpt[pdpt_idx] & ~0x3FFFFFFF) + (vaddr & 0x3FFFFFFF);
	}

	uint64_t *pd = (uint64_t*)phys_to_virt((uint64_t)pdpt[pdpt_idx] & ~0xFFF);
	if (!(pd[pd_idx] & 0x1)) {
		return 0;
	}
	if (PTE_IS_LEAF(pd[pd_idx])) {
		return (pd[pd_idx] & ~0x1FFFFF) + (vaddr & 0x1FFFFF);
	}

	uint64_t *pt = (uint64_t*)phys_to_virt((uint64_t)pd[pd_idx] & ~0xFFF);
	if (!(pt[pt_idx] & 0x1)) {
		return 0;
	}

	return (pt[pt_idx] & ~0xFFF); // 返回页基址
}
EXPORT_SYMBOL(vmm_get_phys);

spinlock_t vmm_lock = SPINLOCK_INIT();

static int __vmm_unmap(uint64_t *pml4, uint64_t vaddr)
{
	/*
	 * vaddr 是虚拟地址，只需清掉页内偏移 12 位；PTE_ADDR_MASK 是物理帧号
	 * 掩码 (0x000ffffffffff000)，套用到虚拟地址会把高位 sign-extension
	 * 的 0xfff 砍成 0x000，破坏 -2GB 区 (0xffffffff80000000+) 的索引。
	 */
	vaddr &= ~(uint64_t)(PAGE_SIZE - 1);

	// 逐级向下查找，如果中途发现某一级不存在，说明该地址本来就没映射
	uint64_t pml4_i = PML4_IDX(vaddr);
	if (!(pml4[pml4_i] & PTE_PRESENT)) {
		L("unmaped 4 page %p\n", (void*)vaddr);
		return -1;
	}
	uint64_t *pdpt = (uint64_t*)phys_to_virt(pml4[pml4_i] & PTE_ADDR_MASK);

	uint64_t pdpt_i = PDPT_IDX(vaddr);
	if (!(pdpt[pdpt_i] & PTE_PRESENT)) {
		L("unmaped 3 page %p\n", (void*)vaddr);
		return -1;
	}
	uint64_t *pd = (uint64_t*)phys_to_virt(pdpt[pdpt_i] & PTE_ADDR_MASK);

	uint64_t pd_i = PD_IDX(vaddr);
	if (!(pd[pd_i] & PTE_PRESENT)) {
		L("unmaped 2 page %p\n", (void*)vaddr);
		return -1;
	}
	uint64_t *pt = (uint64_t*)phys_to_virt(pd[pd_i] & PTE_ADDR_MASK);

	uint64_t pt_i = PT_IDX(vaddr);

	// 清空条目
	pt[pt_i] = 0;

	// 必须刷新 TLB 否则 CPU 可能会继续使用缓存的旧物理地址映射，导致写入错误内存
	arch_tlb_flush_one(vaddr);

	return 0;
}

int vmm_unmap(uint64_t *pml4, uint64_t vaddr)
{
	uint64_t flags = 0;
	arch_spin_lock_irqsave(&vmm_lock, flags);

	int n = __vmm_unmap(pml4, vaddr);

	arch_spin_unlock_irqrestore(&vmm_lock, flags);

	if (0 == n) {
		ipi_broadcast(IPI_VECTOR_TLB);
	}

	return n;
}
EXPORT_SYMBOL(vmm_unmap);

uint64_t *kernel_pml4 = NULL;
EXPORT_SYMBOL(kernel_pml4);

static uint64_t vmm_init_ap()
{
	// AP 不需要重新克隆 Limine 的页表
	// 而是直接使用 BSP 已经准备好的那个终极页表
	uint64_t pml4_phys = virt_to_phys(kernel_pml4);

	return pml4_phys;
}

static uint64_t vmm_init_bsp()
{
	// 1. 申请物理页并转换为虚拟地址作为新 PML4
	uint64_t phys_pml4 = (uint64_t)pmm_alloc();
	kernel_pml4 = (uint64_t*)phys_to_virt(phys_pml4);

	kprintf("[vmm] new PML4 phys=%p virt=%p hhdm_off=0x%lx\n",
			phys_pml4, kernel_pml4, hhdm_offset());

	// 先清空新页表（仅清空用户空间 0-255，保留后续要写入的内核空间）
	memset(kernel_pml4, 0, PAGE_SIZE);

	// 2. 获取当前 Limine 正在使用的物理页表地址
	uint64_t old_pml4_phys = arch_read_cr3();
	uint64_t *old_pml4_virt = (uint64_t*)phys_to_virt(old_pml4_phys);

	/* 3. 克隆恒等映射 (identity map, PML4[0])。
	 * 旧页表可能只映射了 0-2MB，新 PML4 在 >=4MB 处，此时 __vmm_map
	 * 会通过旧页表分配本级缺失的 PD/PT 并填入新 PDE/PTE。 */
	if (old_pml4_virt[0] != 0)
		kernel_pml4[0] = old_pml4_virt[0];

	// 4. 克隆内核映射和 HHDM (PML4[256-511])
	// x86_64 PML4 有 512 个条目，0-255 通常是用户态，256-511 是内核态，
	// Limine 会把内核和 HHDM 放在 256-511 范围内。
	for (int i = 256; i < 512; i++) {
		if (old_pml4_virt[i] != 0) {
			kernel_pml4[i] = old_pml4_virt[i];
		}
	}

	/* 5. 确保新 PML4 自身在恒等映射下可被访问：
	 * 当 hhdm_offset==0 时，内核通过恒等映射访问物理地址。
	 * 若新 PML4 物理页不在旧恒等映射范围内，通过旧页表补齐缺失
	 * 层级并映射到自身物理地址。vmm_map 在锁内调用 __vmm_map。 */
	vmm_map(old_pml4_virt, phys_pml4, phys_pml4,
			PTE_PRESENT | PTE_WRITABLE);

	return phys_pml4;
}

/**
 * vmm_preallocate_kernel_range()
 *
 * 任何时候创建用户进程，拷贝的 256 个条目都指向了固定的内核 PDPT 页面。
 * 之后内核无论怎么 vmm_map(只要在 256-511 范围内)，都只会在 PDPT
 * 及其下级操作，所有进程的 PML4 都无需再次同步。
 *
 * 调用时序保证：BSP 先完成 vmm_init_bsp + 本函数填充 PML4[256..511]，
 * 之后才逐个启动 AP；AP 调用时所有条目已为 PRESENT，循环体是 no-op，
 * 因此无需额外加锁。
 */
static void vmm_preallocate_kernel_range()
{
	// 遍历内核空间的 PML4 范围 (256-511)
	for (int i = 256; i < 512; i++) {
		if (!(kernel_pml4[i] & PTE_PRESENT)) {
			uint64_t new_pt = (uint64_t)pmm_alloc();
			if (!new_pt) {
				continue;
			}
			memset(phys_to_virt(new_pt), 0, PAGE_SIZE);
			// 预先填入，确保 PML4 层级不再变动
			kernel_pml4[i] = new_pt | PTE_PRESENT | PTE_WRITABLE | VMM_TABLE_EXTRA_FLAGS;
		}
	}
}

void vmm_init(int flag)
{
	if (!flag) {
		L("Building kernel pml4.");
	}
	uint64_t phys_pml4 = flag ? vmm_init_ap() : vmm_init_bsp();

	// 4. 切换到新页表
	L("VMM: switching page-table root to %p", (void*)phys_pml4);
	arch_switch_page_table_root(phys_pml4);

	/* arch_switch_page_table_root() switches the architecture's root and
	 * performs the ordering required before subsequent translations. */

	L("CPU %u Context switched, global kernel_pml4 points to %p.", flag, kernel_pml4);

	if (!flag) {
		struct limine_framebuffer *framebuffer = (struct limine_framebuffer*)fb_get_info();
		(void)framebuffer;
		vmm_test_secret_base();
	}

	vmm_preallocate_kernel_range();
}

/**
 * __vmm_map() - 将物理地址映射到虚拟地址
 *
 * pml4: 页表根节点的虚拟地址。
 * vaddr/paddr: 待映射的虚拟和物理地址。
 * flags: 权限标志位，调用方必须包含 PTE_PRESENT，否则映射静默失效。
 * overwrite_ok: true=重复映射时警告并覆盖，false=重复映射时 panic。
 *
 * 返回值：0=成功，-1=pmm_alloc 失败(中间页表分配 OOM)。
 */
static int __vmm_map(uint64_t *pml4, uint64_t vaddr, uint64_t paddr,
		uint64_t flags, bool overwrite_ok)
{
	/*
	 * 虚拟地址只需页对齐；不要套 PTE_ADDR_MASK，否则会把 -2GB 区
	 * (0xffffffff80000000+) 高位的 sign-extension 砍掉，导致索引算错。
	 */
	vaddr &= ~(uint64_t)(PAGE_SIZE - 1);
	paddr &= PTE_ADDR_MASK;

	// 第 4 级 (PML4) -> 第 3 级 (PDPT)
	uint64_t pml4_i = PML4_IDX(vaddr);
	if (!(pml4[pml4_i] & PTE_PRESENT)) {
		uint64_t new_pt = (uint64_t)pmm_alloc();
		if (new_pt == 0) {
			return -1;
		}
		memset(phys_to_virt(new_pt), 0, PAGE_SIZE);
		// 新建中间层时，至少赋予 Present 和 Writable 权限
		// 如果 flags 包含 User 位，中间层也应该包含 User 位
		pml4[pml4_i] = new_pt | PTE_PRESENT | PTE_WRITABLE | VMM_TABLE_EXTRA_FLAGS | PTE_TABLE_USER_MASK(flags);
	} else {
		// 即使该层已存在，也必须确保它是可写的，特别是针对 Limine 预设的 HHDM 路径
		pml4[pml4_i] |= PTE_WRITABLE;
		uint64_t _tu = PTE_TABLE_USER_MASK(flags); if (_tu) pml4[pml4_i] |= _tu;
	}
	uint64_t *pdpt = (uint64_t*)phys_to_virt(pml4[pml4_i] & PTE_ADDR_MASK);

	// 第 3 级 (PDPT) -> 第 2 级 (PD)
	uint64_t pdpt_i = PDPT_IDX(vaddr);
	if (!(pdpt[pdpt_i] & PTE_PRESENT)) {
		uint64_t new_pt = (uint64_t)pmm_alloc();
		if (new_pt == 0) {
			return -1;
		}
		memset(phys_to_virt(new_pt), 0, PAGE_SIZE);
		pdpt[pdpt_i] = new_pt | PTE_PRESENT | PTE_WRITABLE | VMM_TABLE_EXTRA_FLAGS | PTE_TABLE_USER_MASK(flags);
	} else {
		/*
		 * 1GB huge-page leaf -> split into 512 x 2MB L2 blocks.
		 * x86_64 Limine HHDM may use 1GB pages; ARM64 currently doesn't
		 * but the logic is arch-neutral thanks to PTE_TABLE_USER_MASK.
		 */
		if (PTE_IS_LEAF(pdpt[pdpt_i])) {
			uint64_t block_base  = pdpt[pdpt_i] & PTE_ADDR_MASK;
			uint64_t block_attrs = pdpt[pdpt_i] & ~PTE_ADDR_MASK;
			uint64_t new_pt = (uint64_t)pmm_alloc();
			if (new_pt == 0) return -1;
			uint64_t *l2 = (uint64_t *)phys_to_virt(new_pt);
			memset(l2, 0, PAGE_SIZE);
			for (int i = 0; i < 512; i++) {
				l2[i] = (block_base + i * 0x200000ULL) | block_attrs
					| VMM_LEAF_EXTRA_FLAGS;
			}
			pdpt[pdpt_i] = new_pt | PTE_PRESENT | PTE_WRITABLE
				| VMM_TABLE_EXTRA_FLAGS | PTE_TABLE_USER_MASK(flags);
		}
		// 确保路径可写
		pdpt[pdpt_i] |= PTE_WRITABLE;
		uint64_t _tu2 = PTE_TABLE_USER_MASK(flags);
		if (_tu2) {
			pdpt[pdpt_i] |= _tu2;
		}
	}
	uint64_t *pd = (uint64_t*)phys_to_virt(pdpt[pdpt_i] & PTE_ADDR_MASK);

	// 第 2 级 (PD) -> 第 1 级 (PT)
	uint64_t pd_i = PD_IDX(vaddr);
	if (!(pd[pd_i] & PTE_PRESENT)) {
		uint64_t new_pt = (uint64_t)pmm_alloc();
		if (new_pt == 0) {
			return -1;
		}
		memset(phys_to_virt(new_pt), 0, PAGE_SIZE);
		pd[pd_i] = new_pt | PTE_PRESENT | PTE_WRITABLE | VMM_TABLE_EXTRA_FLAGS | PTE_TABLE_USER_MASK(flags);
	} else {
		/*
		 * M3: 2MB block leaf → 拆分为 512×4KB page table entries.
		 * ARM64 identity mapping 的初始阶段用 2MB block 映射 DRAM，
		 * EL0 用户态需要细粒度 4KB 映射才能逐页控制 PTE_USER。
		 */
		if (PTE_IS_LEAF(pd[pd_i])) {
			uint64_t block_base  = pd[pd_i] & PTE_ADDR_MASK;
			uint64_t block_attrs = pd[pd_i] & ~PTE_ADDR_MASK;
			uint64_t new_pt = (uint64_t)pmm_alloc();
			if (new_pt == 0) {
				return -1;
			}
			uint64_t *l3 = (uint64_t*)phys_to_virt(new_pt);
			memset(l3, 0, PAGE_SIZE);
			/*
			 * 512 entries: each maps a contiguous 4KB sub-page,
			 * inheriting ALL attributes from the original block
			 * (AttrIndx, AP, SH, AF, nG etc. — NOT just PTE_USER|PTE_NX).
			 */
			for (int i = 0; i < 512; i++) {
				l3[i] = (block_base + i * 0x1000) | block_attrs
					| VMM_LEAF_EXTRA_FLAGS;
			}
			/* Replace block descriptor with table descriptor */
			pd[pd_i] = new_pt | PTE_PRESENT | PTE_WRITABLE
				| VMM_TABLE_EXTRA_FLAGS | PTE_TABLE_USER_MASK(flags);
			/* Invalidate the old 2MB-block TLB entry */
			arch_tlb_flush_one(vaddr);
		}
		// 确保路径可写
		pd[pd_i] |= PTE_WRITABLE;
		uint64_t _tu2 = PTE_TABLE_USER_MASK(flags);
		if (_tu2) {
			pd[pd_i] |= _tu2;
		}
	}
	uint64_t *pt = (uint64_t*)phys_to_virt(pd[pd_i] & PTE_ADDR_MASK);

	// 第 1 级 (PT) -> 最终物理页
	uint64_t pt_i = PT_IDX(vaddr);

	// 重复映射检测
	if (pt[pt_i] & PTE_PRESENT) {
		if (overwrite_ok) {
			L("VMM: Overwriting mapping at %p\n", (void*)vaddr);
		} else {
			panic("VMM: Overwriting mapping at %p "
					"(pml4[%lu]=0x%lx pdpt[%lu]=0x%lx pd[%lu]=0x%lx pt[%lu]=0x%lx)\n",
					(void*)vaddr, pml4_i, pml4[pml4_i], pdpt_i, pdpt[pdpt_i],
					pd_i, pd[pd_i], pt_i, pt[pt_i]);
		}
	}

	// 设置最后一级页表项，直接应用传入的 flags
	pt[pt_i] = paddr | flags | VMM_LEAF_EXTRA_FLAGS;

	// 刷新 TLB (仅刷新当前页以提高性能)
	arch_tlb_flush_one(vaddr);

	return 0;
}

int vmm_remap(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
	uint64_t flag = 0;
	arch_spin_lock_irqsave(&vmm_lock, flag);

	int n = __vmm_map(pml4, vaddr, paddr, flags, true);

	arch_spin_unlock_irqrestore(&vmm_lock, flag);

	/* 重映射改变已有映射，其他核可能缓存了旧 PTE，必须 IPI 击落 */
	ipi_broadcast(IPI_VECTOR_TLB);

	return n;
}
EXPORT_SYMBOL(vmm_remap);

int vmm_map(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
	uint64_t flag = 0;
	arch_spin_lock_irqsave(&vmm_lock, flag);

	int n = __vmm_map(pml4, vaddr, paddr, flags, false);

	arch_spin_unlock_irqrestore(&vmm_lock, flag);

	/* 纯新映射，其他核从未缓存该地址，无需 IPI 击落 */
	return n;
}
EXPORT_SYMBOL(vmm_map);

int vmm_map_global(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
	// 1. 修改全局页表 (kernel_pml4)
	// __vmm_map 内部已 invlpg 刷本核 TLB，无需重复
	if (-1 == vmm_map(kernel_pml4, vaddr, paddr, flags)) {
		return -1;
	}

	// 2. 通知其他核心刷新(IPI 击落)
	ipi_broadcast(IPI_VECTOR_TLB);

	return 0;
}
EXPORT_SYMBOL(vmm_map_global);

/**
 * vmm_map_region() - 将一段连续的物理内存映射到连续的虚拟内存
 *
 * pml4: 内核页表根指针(虚拟地址)。
 * vaddr/paddr/size: 映射区域。
 * flags: 页表项权限位(如 PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE).
 */
int vmm_map_region(uint64_t *pml4, uint64_t vaddr, uint64_t paddr,
		uint64_t size, uint64_t flags)
{
	// 1. 自动处理基地址不对齐的情况(向下对齐)
	uint64_t v_start = vaddr & ~((uint64_t)(PAGE_SIZE - 1));
	uint64_t p_start = paddr & ~((uint64_t)(PAGE_SIZE - 1));

	// 2. 重新计算包含不对齐部分在内的总长度(向上取整到 PAGE_SIZE)
	uint64_t end_vaddr = vaddr + size;
	uint64_t aligned_size = ((end_vaddr + (PAGE_SIZE - 1)) &
			~((uint64_t)(PAGE_SIZE - 1))) - v_start;

	uint64_t flag = 0;
	arch_spin_lock_irqsave(&vmm_lock, flag);

	// 3. 步进量必须严格等于页大小
	for (uint64_t offset = 0; offset < aligned_size; offset += PAGE_SIZE) {
		// 物理和虚拟同步增加同样的 offset
		if (__vmm_map(pml4, v_start + offset, p_start + offset,
					flags, false) < 0) {
			arch_spin_unlock_irqrestore(&vmm_lock, flag);
			return -1;
		}
	}

	arch_spin_unlock_irqrestore(&vmm_lock, flag);

	/* 纯新映射，无需 IPI 击落:__vmm_map 内 arch_tlb_flush_one 已覆盖本核 */
	return 0;
}

uint64_t *vmm_create_user_pml4(void)
{
	// 1. 申请一个物理页作为新的 PML4 表
	uint64_t *new_pml4_phys = pmm_alloc();

	// 2. 获取其虚拟地址以便对其进行写操作
	uint64_t *new_pml4_virt = (uint64_t*)phys_to_virt((uint64_t)new_pml4_phys);

	// 3. 先全部清空
	memset(new_pml4_virt, 0, PAGE_SIZE);

	// 4. 取内核页表的虚拟地址。
	// 不能直接读 CR3:若正处在 user1 上下文中创建 user2，CR3 指向的是 user1 的
	// 物理 PML4 而非内核页表；统一通过全局 kernel_pml4 取，与当前上下文无关。
	extern uint64_t *kernel_pml4;
	uint64_t current_cr3 = virt_to_phys(kernel_pml4);

	// 屏蔽低 12 位 PCID / 标志位
	current_cr3 &= ~0xFFFULL;

	// 转换为虚拟地址
	uint64_t *k_pml4_virt = (uint64_t*)phys_to_virt(current_cr3);

	// 5. 复制内核空间映射(高 256 个 PML4 条目).
	// x86_64 的后 256 个 PML4 条目对应高地址空间(内核 + HHDM).
	for (int i = 256; i < 512; i++) {
		new_pml4_virt[i] = k_pml4_virt[i];
	}

	// 返回物理地址，因为 CR3 寄存器需要物理地址
	return new_pml4_phys;
}

void vmm_dump_pml4(uint64_t pml4_phys, const char *name)
{
	uint64_t *pml4 = (uint64_t*)phys_to_virt(pml4_phys);
	kprintf("[dump] %s pml4=%p\n", name, pml4_phys);
	for (int i = 0; i < 4; i++) { /* 只看低地址的几个条目 */
		if (!pml4[i]) {
			continue;
		}
		uint64_t pdpt_phys = pml4[i] & ~0xFFFULL;
		kprintf("[dump]   PML4[%d]=%p\n", i, pml4[i]);
		uint64_t *pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
		for (int j = 0; j < 4; j++) {
			if (!pdpt[j]) {
				continue;
			}
			uint64_t pd_phys = pdpt[j] & ~0xFFFULL;
			kprintf("[dump]     PDPT[%d]=%p\n", j, pdpt[j]);
			uint64_t *pd = (uint64_t*)phys_to_virt(pd_phys);
			for (int k = 0; k < 4; k++) {
				if (!pd[k]) {
					continue;
				}
				kprintf("[dump]       PD[%d]=%p\n", k, pd[k]);
			}
		}
	}
}

void vmm_test_secret_base(void)
{
	uint64_t secret_vaddr = 0x10000000000;
	uint64_t phys_page = (uint64_t)pmm_alloc();

	if (phys_page == 0) {
		L("VMM ERROR: Failed to allocate page for secret base");
		return;
	}

	// 添加 PTE_PRESENT 标志
	__vmm_map(kernel_pml4, secret_vaddr, phys_page, PTE_PRESENT | PTE_WRITABLE, false);

	L("VMM: Writing to secret base at %p...", (void*)secret_vaddr);
	uint64_t *ptr = (uint64_t*)secret_vaddr;
	*ptr = 0xCAFEBABE;

	if (*ptr == 0xCAFEBABE) {
		L("VMM: Secret base works! Data: %lx", *ptr);
	}

	uint64_t alias_vaddr = 0x20000000000;
	// 添加 PTE_PRESENT 标志
	__vmm_map(kernel_pml4, alias_vaddr, phys_page, PTE_PRESENT | PTE_WRITABLE, false);

	L("VMM: Alias check: %lx", *(uint64_t*)alias_vaddr);
}

// flags 必须包含 PTE_USER;PTE_PRESENT 由调用方保证
int vmm_map_user(uint64_t* pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
	// 强制加入 PTE_USER，确保 PML4E/PDPTE/PDE/PTE 全路径可被 Ring 3 访问。
	return vmm_map((uint64_t*)phys_to_virt((uint64_t)pml4_phys), vaddr, paddr,
			flags | PTE_USER);
}

void vmm_map_specific(uint64_t *pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags)
{

	vmm_map((uint64_t*)phys_to_virt((uint64_t)pml4_phys), vaddr, paddr, flags);
}

/**
 * vmm_destroy_level() - 递归销毁页表结构并释放物理页
 *
 * table_phys: 页表物理地址，调用方保证其 phys_to_virt 映射存在。
 * level: 当前层级编号，4=PML4, 3=PDPT,2=PD,1=PT。
 *   level 从 4 向下递归至 1，逐层解析 entry -> 递归下一级 -> 释放本级物理页。
 *
 * 调用方必须保证独占该页表树，函数内部不持任何锁，并发 vmm_map 会导致
 * 竞争(entry 可能在检查 PRESENT 后，pmm_free 前被并发修改)。
 *
 * 返回值：无。销毁后页表树不可用，相关 TLB 项需调用方手动刷新。
 */
void vmm_destroy_level(uint64_t table_phys, int level)
{
	uint64_t *table = (uint64_t*)phys_to_virt((uint64_t)table_phys);

	// 每一层有 512 个条目
	// 如果是 PML4 (level 4)，我们只处理低 256 个条目(用户空间)
	int limit = (level == 4) ? 256 : 512;

	for (int i = 0; i < limit; i++) {
		uint64_t entry = table[i];

		// 只有 Present 位为 1 的条目才有效
		if (!(entry & PTE_PRESENT)) {
			continue;
		}

		if (level == 1) {
			// 如果是第 1 层 (PT)，条目指向的就是实际的用户数据物理页
			pmm_free((void*)(entry & PTE_ADDR_MASK));
		} else if (PTE_IS_LEAF(entry)) {
			// 叶节点（2MB/1GB 大页 block descriptor）
			/* pmm_free_pages 按页数全量释放 bitmap 位。
			 * Level 2 (PD): 2MB = 512 页。Level 3 (PDPT): 1GB = 262144 页。 */
			uint64_t pg_count = (level == 3) ? (1ULL << 18) : 512;
			pmm_free_pages((void*)(entry & PTE_ADDR_MASK), pg_count);
		} else {
			// 中间层表描述符：递归进入下一级
			vmm_destroy_level(entry & PTE_ADDR_MASK, level - 1);
		}
	}

	// 释放当前这一层页表本身占用的物理页
	pmm_free((void*)table_phys);
}

void vmm_test_framebuffer(void *_fb)
{
	struct limine_framebuffer *fb = _fb;

	uint64_t fb_phys = virt_to_phys(fb->address);
	uint64_t my_secret_fb = 0xffffffffc0000000; // 定义一个好记的虚拟地址

	// 映射足够的长度 (比如 4MB)
	for (uint64_t i = 0; i < 1024; i++) {
		__vmm_map(kernel_pml4, my_secret_fb + (i * PAGE_SIZE),
				fb_phys + (i * PAGE_SIZE), PTE_PRESENT | PTE_WRITABLE, false);
	}

	uint32_t *fb_ptr = (uint32_t*)my_secret_fb;
	for (int i = 0; i < 2000; i++) {
		// 画一排绿点
		fb_ptr[i] = 0x00FF00;
	}

	kprintf("VMM: Drawing green to secret FB at %p...\n", my_secret_fb);
	for (uint64_t i = 0; i < 1024; i++) {
		__vmm_unmap(kernel_pml4, my_secret_fb + (i * PAGE_SIZE));
	}
}

// 实验代码示例
void vmm_test(void)
{
	uint64_t phys = (uint64_t)pmm_alloc();
	kprintf("TEST START: kernel_pml4=%p, phys_alloc=%p\n", kernel_pml4, phys);

	__vmm_map(kernel_pml4, 0x12345678000, phys, PTE_PRESENT | PTE_WRITABLE, false);

	uint64_t cr3_now = arch_read_cr3();
	kprintf("TEST MID: Current CR3=%p\n", cr3_now);

	uint64_t* test_ptr = (uint64_t*)0x12345678000;
	kprintf("TEST: Attempting write to %p\n", test_ptr);
	*test_ptr = 0xABCDE; // 如果在这里崩，说明映射没成功
	kprintf("TEST: Write success! Value: %x\n", *test_ptr);

	__vmm_unmap(kernel_pml4, 0x12345678000);
#if CONFIG_VMM_TEST
	kprintf("VMM: Unmapped. Next access should cause Page Fault...\n");
	*test_ptr = 0x0; // 此时应该触发 EXCEPTION 14
	kprintf("TEST: Write should error! Value: %x\n", *test_ptr);
#else
	kprintf("VMM: Unmapped. Skipping self-destruct (CONFIG_VMM_TEST=0).\n");
#endif
}
