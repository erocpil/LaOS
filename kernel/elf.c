/*
 * elf.c — ELF 用户程序加载与用户栈初始化
 *
 * 验证 ELF 文件头，将程序段映射到地址空间，并构造用户栈参数区。
 */

#include "elf.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "define.h"
#include "string.h"
#include "hhdm.h"
#include "printf.h"

int elf_check(Elf64_Ehdr *ehdr)
{
	if (!ehdr) {
		kprintf("[elf] invalid null ELF header\n");
		return -1;
	}

	if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' ||
		ehdr->e_ident[3] != 'F') {
		kprintf("[elf] invalid ELF magic\n");
		return -1;
	}

	if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
		kprintf("[elf] unsupported ELF class %u (expected 64-bit)\n",
			ehdr->e_ident[EI_CLASS]);
		return -1;
	}

	if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
		kprintf("[elf] unsupported ELF data encoding %u (expected little-endian)\n",
			ehdr->e_ident[EI_DATA]);
		return -1;
	}

	if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
		kprintf("[elf] unsupported ELF ident version %u\n", ehdr->e_ident[EI_VERSION]);
		return -1;
	}

	if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_REL) {
		kprintf("[elf] unsupported ELF type %u\n", ehdr->e_type);
		return -1;
	}

	if (ehdr->e_machine != EM_X86_64 && ehdr->e_machine != EM_AARCH64) {
		kprintf("[elf] unsupported ELF machine %u (expected x86_64 or AArch64)\n",
			ehdr->e_machine);
		return -1;
	}

	if (ehdr->e_ehsize != sizeof(Elf64_Ehdr)) {
		kprintf("[elf] invalid ELF header size %u (expected %u)\n", ehdr->e_ehsize,
			(unsigned int)sizeof(Elf64_Ehdr));
		return -1;
	}

	if (ehdr->e_type == ET_EXEC && ehdr->e_phoff == 0) {
		kprintf("[elf] ET_EXEC is missing a program header table\n");
		return -1;
	}

	if (ehdr->e_type == ET_REL && ehdr->e_shoff == 0) {
		kprintf("[elf] ET_REL is missing a section header table\n");
		return -1;
	}

	return 0;
}

/* 根据程序段标志设置映射权限。内容经 HHDM 写入物理页，不通过用户虚拟地址
 * 访问，因此本函数不管理 SMAP 访问窗口。 */
bool elf_load_segment(uint64_t *pml4_phys, Elf64_Phdr *phdr, uint8_t *elf_raw, int is_user)
{
	uint64_t vaddr = phdr->p_vaddr;
	uint64_t filesz = phdr->p_filesz;
	uint64_t memsz = phdr->p_memsz;
	uint64_t file_offset = phdr->p_offset;

	// 使用架构定义的权限标志，保持 x86_64 与 AArch64 的映射语义一致。
	uint64_t flags = PTE_PRESENT | PTE_MEMATTR_NORMAL;
	if (phdr->p_flags & PF_W) {
		flags |= PTE_WRITABLE;
	}
	if (is_user) {
		flags |= PTE_USER;
	}
	if (!(phdr->p_flags & PF_X)) {
		flags |= PTE_NX;
	}

	// 计算覆盖程序段的页对齐范围。
	uint64_t vaddr_page_base = vaddr & ~0xFFFULL;
	uint64_t offset_in_page = vaddr & 0xFFFULL;
	uint64_t num_pages = (vaddr + memsz - vaddr_page_base + (PAGE_SIZE - 1)) / PAGE_SIZE;

	// 逐页分配或复用映射，并填充文件内容及零初始化区域。
	for (uint64_t i = 0; i < num_pages; i++) {
		uint64_t page_vaddr = vaddr_page_base + (i * PAGE_SIZE);
		/* 仅保留物理地址位；已有映射的权限位不应被误认为地址的一部分。 */
		uint64_t phys = vmm_get_phys((uint64_t)pml4_phys, page_vaddr) & PTE_ADDR_MASK;
		if (phys == 0) {
			// 页面尚未映射，分配物理页并建立映射。
			phys = (uint64_t)pmm_alloc();
			if (!phys) {
				return false;
			}
			vmm_map_specific((uint64_t *)pml4_phys, page_vaddr, phys, flags);
		}

		// 经 HHDM 写入目标物理页，无需切换当前页表。
		uint64_t dest_virt = (uint64_t)phys_to_virt(phys);
		uint64_t copy_start = (i == 0) ? offset_in_page : 0;
		// 计算本页可写入的字节数。
		uint64_t space_in_page = PAGE_SIZE - copy_start;

		// 计算本页中属于该程序段的范围，包括需要清零的 BSS 区域。
		uint64_t current_pos = page_vaddr + copy_start;
		uint64_t fill_size = 0;
		if (vaddr + memsz > current_pos) {
			fill_size = vaddr + memsz - current_pos;
			if (fill_size > space_in_page) {
				fill_size = space_in_page;
			}
		}

		// 计算本页中来自 ELF 文件的字节数。
		uint64_t copy_size = 0;
		if (vaddr + filesz > current_pos) {
			copy_size = vaddr + filesz - current_pos;
			if (copy_size > fill_size) {
				copy_size = fill_size;
			}
		}

		if (fill_size > 0) {
			memset((void *)(dest_virt + copy_start), 0, fill_size);
			if (copy_size > 0) {
				uint64_t src_offset = file_offset + (current_pos - vaddr);
				memcpy((void *)(dest_virt + copy_start), elf_raw + src_offset,
					copy_size);
			}
		}
	}

	return true;
}

/* 初始化用户栈并填充 argc/argv。
 *
 * pml4_phys: 进程页表物理地址。
 * argc/argv: 参数计数与数组。
 *
 * 返回值：用户态的 RSP 虚拟地址。
 * 内核通过 HHDM 写入物理页；栈中保存的参数指针必须是用户虚拟地址。
 */
uint64_t setup_user_stack(uint64_t pml4_phys, int argc, char **argv, uint64_t *phys_out)
{
	uint64_t stack_page_phys;
	uint8_t *stack;
	uint8_t *k_stack_base;
	uint8_t *current_k_ptr;
	uint64_t current_user_vaddr;
	uint64_t *arg_vaddrs;
	uint64_t user_stack_top = USER_STACK_TOP;
	uint64_t ptr_bytes;
	uint64_t cells;

	if (phys_out) {
		*phys_out = 0;
	}
	if (argc < 1 || !argv) {
		return 0;
	}
	if ((uint64_t)argc > UINT64_MAX / sizeof(*arg_vaddrs)) {
		return 0;
	}
	ptr_bytes = (uint64_t)argc * sizeof(*arg_vaddrs);
	/* argv 指针、argc 和终止用的 NULL 所需的栈槽数。 */
	if ((uint64_t)argc > UINT64_MAX / sizeof(uint64_t) - 2) {
		return 0;
	}
	cells = (uint64_t)argc + 2;

	stack_page_phys = (uint64_t)pmm_alloc();
	if (!stack_page_phys) {
		return 0;
	}

	// 将栈页映射为用户可写页面。
	if (vmm_map_user((uint64_t *)pml4_phys, user_stack_top - PAGE_SIZE, stack_page_phys,
		    PTE_PRESENT | PTE_WRITABLE | PTE_MEMATTR_NORMAL) != 0) {
		pmm_free((void *)stack_page_phys);
		return 0;
	}
	if (phys_out) {
		*phys_out = stack_page_phys;
	}

	// 经 HHDM 访问栈页；k_stack_base 指向该页末端，作为向下增长的栈起点。
	stack = (uint8_t *)phys_to_virt(stack_page_phys);
	k_stack_base = stack + PAGE_SIZE;

	// 同步维护内核映射地址与用户栈虚拟地址。
	current_user_vaddr = user_stack_top;
	current_k_ptr = k_stack_base;

	// 暂存每个参数字符串的用户虚拟地址，随后写入 argv 数组。
	arg_vaddrs = (uint64_t *)kmalloc(ptr_bytes);
	if (!arg_vaddrs) {
		goto fail;
	}

	// 从末尾参数开始写入字符串，使最终 argv 数组仍按原顺序排列。
	for (int i = argc - 1; i >= 0; i--) {
		size_t len = strlen(argv[i]) + 1;
		if (len > (size_t)(current_k_ptr - stack)) {
			goto fail_args;
		}
		current_k_ptr -= len;
		current_user_vaddr -= len;
		if (current_k_ptr < stack) {
			goto fail_args;
		}

		memcpy(current_k_ptr, argv[i], len);
		arg_vaddrs[i] = current_user_vaddr;
	}

	// 将栈指针对齐到 8 字节边界。
	uint64_t align = current_user_vaddr % 8;
	if (align > (uint64_t)(current_k_ptr - stack)) {
		goto fail_args;
	}
	current_k_ptr -= align;
	current_user_vaddr -= align;
	if (current_k_ptr < stack) {
		goto fail_args;
	}

	// 为 argc、argc 个 argv 指针及末尾 NULL 预留栈空间。
	if (cells > (uint64_t)(current_k_ptr - stack) / sizeof(uint64_t)) {
		goto fail_args;
	}
	current_k_ptr -= cells * sizeof(uint64_t);
	current_user_vaddr -= cells * sizeof(uint64_t);
	if (current_k_ptr < stack) {
		goto fail_args;
	}

	// current_k_ptr 是目标栈位置对应的内核映射地址。
	uint64_t *stack_qwords = (uint64_t *)current_k_ptr;

	stack_qwords[0] = (uint64_t)argc;

	for (int i = 0; i < argc; i++) {
		stack_qwords[i + 1] = arg_vaddrs[i];
	}

	stack_qwords[argc + 1] = 0;

	kfree(arg_vaddrs);

	// 返回用户态栈指针，由架构入口写入返回上下文。
	return current_user_vaddr;

fail_args:
	kfree(arg_vaddrs);

fail:
	vmm_unmap((uint64_t *)phys_to_virt(pml4_phys), user_stack_top - PAGE_SIZE);
	pmm_free((void *)stack_page_phys);
	if (phys_out) {
		*phys_out = 0;
	}

	return 0;
}

/* 按索引获取 section header。 */
inline Elf64_Shdr *elf_section(Elf64_Ehdr *ehdr, uint16_t idx)
{
	return (Elf64_Shdr *)((uint8_t *)ehdr + ehdr->e_shoff) + idx;
}
