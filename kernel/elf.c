/*
 * elf.c - ELF 用户态程序加载与栈初始化
 *
 * 解析 ELF 文件头，加载 segment 到用户空间，设置用户栈与参数。
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

	if (
			ehdr->e_ident[0] != 0x7f ||
			ehdr->e_ident[1] != 'E'  ||
			ehdr->e_ident[2] != 'L'  ||
			ehdr->e_ident[3] != 'F'
	   ) {
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
		kprintf("[elf] unsupported ELF ident version %u\n",
				ehdr->e_ident[EI_VERSION]);
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
		kprintf("[elf] invalid ELF header size %u (expected %u)\n",
				ehdr->e_ehsize, (unsigned int)sizeof(Elf64_Ehdr));
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

/** elf_load_segment() - 辅助函数：将虚拟地址映射到物理页并拷贝数据
 *
 * 权限位 (PTE Flags) 的区分
 *   - 内核线程 (is_user = 0): 映射为 Supervisor 页面。
 *     如果内核开启了 SMEP (Supervisor Mode Execution Prevention),
 *     内核执行这些页面是合法的，但用户态无法访问
 *   - 用户线程 (is_user = 1): 映射为 User 页面。
 *     如果开启了 SMAP，内核在访问这些页面时需要显式 stac 指令，这增加了安全性。
 */
bool elf_load_segment(uint64_t *pml4_phys, Elf64_Phdr *phdr, uint8_t *elf_raw, int is_user)
{
	uint64_t vaddr = phdr->p_vaddr;
	uint64_t filesz = phdr->p_filesz;
	uint64_t memsz = phdr->p_memsz;
	uint64_t file_offset = phdr->p_offset;

	// 确定映射权限 (PTE Flags) — 使用架构定义常量，兼容 x86_64 与 ARM64
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

	// 计算页面对齐边界
	uint64_t vaddr_page_base = vaddr & ~0xFFFULL;
	uint64_t offset_in_page = vaddr & 0xFFFULL;
	uint64_t num_pages = (vaddr + memsz - vaddr_page_base + (PAGE_SIZE - 1)) / PAGE_SIZE;

	// 分配，映射并填充数据
	for (uint64_t i = 0; i < num_pages; i++) {
		uint64_t page_vaddr = vaddr_page_base + (i * PAGE_SIZE);
		// 获取或分配物理页
		uint64_t phys = vmm_get_phys((uint64_t)pml4_phys, page_vaddr);
		if (phys == 0) {
			// 尚未映射
			phys = (uint64_t)pmm_alloc();
			if (!phys)
				return false;
			// 使用通用的 map 函数，传入计算好的 flags
			vmm_map_specific((uint64_t*)pml4_phys, page_vaddr, phys, flags);
		}

		// 通过 HHDM 直接写入物理内存，这样不需要切换 CR3 也能跨页表填充数据，且不影响内核当前运行环境
		uint64_t dest_virt = (uint64_t)phys_to_virt(phys);
		uint64_t copy_start = (i == 0) ? offset_in_page : 0;
		// 计算当前页内还有多少空间可以操作
		uint64_t space_in_page = PAGE_SIZE - copy_start;

		// 计算 BSS/数据 总共要填充的大小 (fill_size)
		uint64_t current_pos = page_vaddr + copy_start;
		uint64_t fill_size = 0;
		if (vaddr + memsz > current_pos) {
			fill_size = vaddr + memsz - current_pos;
			if (fill_size > space_in_page) {
				fill_size = space_in_page;
			}
		}

		// 计算从 ELF 文件实际拷贝的大小 (copy_size)
		uint64_t copy_size = 0;
		if (vaddr + filesz > current_pos) {
			copy_size = vaddr + filesz - current_pos;
			if (copy_size > fill_size) {
				copy_size = fill_size;
			}
		}

		// 执行内存操作
		if (fill_size > 0) {
			memset((void*)(dest_virt + copy_start), 0, fill_size);
			if (copy_size > 0) {
				uint64_t src_offset = file_offset + (current_pos - vaddr);
				memcpy((void*)(dest_virt + copy_start), elf_raw + src_offset, copy_size);
			}
		}
	}

	return true;
}

/** setup_user_stack() - 初始化用户栈并填充 argc/argv
 *
 * pml4_phys: 进程页表物理地址。
 * argc/argv: 参数计数与数组。
 *
 * 返回值：用户态的 RSP 虚拟地址。
 * 在实现 setup_user_stack 时，最关键的逻辑是：在内核中通过
 * HHDM(物理内存直接映射)操作物理页，但在页内填充的指针必须是用户态的虚拟地址。
 */
uint64_t setup_user_stack(uint64_t pml4_phys, int argc, char **argv,
		uint64_t *phys_out)
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
	/* argc pointers, argc itself, and the terminating NULL. */
	if ((uint64_t)argc > UINT64_MAX / sizeof(uint64_t) - 2) {
		return 0;
	}
	cells = (uint64_t)argc + 2;

	stack_page_phys = (uint64_t)pmm_alloc();
	if (!stack_page_phys) {
		return 0;
	}

	// 映射该页到用户空间 (User | Write | Present | Normal MemAttr)
	if (vmm_map_user((uint64_t*)pml4_phys, user_stack_top - PAGE_SIZE,
				stack_page_phys,
				PTE_PRESENT | PTE_WRITABLE | PTE_MEMATTR_NORMAL) != 0) {
		pmm_free((void*)stack_page_phys);
		return 0;
	}
	if (phys_out) {
		*phys_out = stack_page_phys;
	}

	// 在内核中通过 HHDM 访问该页
	// k_stack_base 指向该页在内核中的最高处(模拟栈底)
	stack = (uint8_t*)phys_to_virt(stack_page_phys);
	k_stack_base = stack + PAGE_SIZE;

	// 记录当前操作在用户态对应的虚拟地址偏移
	current_user_vaddr = user_stack_top;
	current_k_ptr = k_stack_base;

	// 临时存储字符串在用户态的地址，稍后填入指针数组
	arg_vaddrs = (uint64_t*)kmalloc(ptr_bytes);
	if (!arg_vaddrs) {
		goto fail;
	}

	// 1. 压入实际的字符串数据 (String Area)
	// 按照从后往前的顺序压入，这样指针数组顺序更直观
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
		// 记录字符串在用户空间的虚拟地址
		arg_vaddrs[i] = current_user_vaddr;
	}

	// 2. 8 字节对齐
	// x86_64 栈指针 RSP 必须在压入数据前对齐
	uint64_t align = current_user_vaddr % 8;
	if (align > (uint64_t)(current_k_ptr - stack)) {
		goto fail_args;
	}
	current_k_ptr -= align;
	current_user_vaddr -= align;
	if (current_k_ptr < stack) {
		goto fail_args;
	}

	// 3. 压入指针数组和 argc (stack_qwords 功能)
	// 计算需要预留的空间:1(NULL) + argc(指针) + 1(argc本身)
	// 每一项都是 8 字节
	if (cells > (uint64_t)(current_k_ptr - stack) / sizeof(uint64_t)) {
		goto fail_args;
	}
	current_k_ptr -= cells * sizeof(uint64_t);
	current_user_vaddr -= cells * sizeof(uint64_t);
	if (current_k_ptr < stack) {
		goto fail_args;
	}

	// 使用 stack_qwords 视图来操作这块内存
	// 此时 current_k_ptr 指向的是准备给用户态 RSP 的内核映射地址
	uint64_t *stack_qwords = (uint64_t*)current_k_ptr;

	// 布局索引 0: argc
	stack_qwords[0] = (uint64_t)argc;

	// 布局索引 1 ~ argc: argv[0] ... argv[n-1]
	for (int i = 0; i < argc; i++) {
		stack_qwords[i + 1] = arg_vaddrs[i];
	}

	// 布局索引 argc + 1: NULL (argv 结束标志)
	stack_qwords[argc + 1] = 0;

	kfree(arg_vaddrs);

	// 返回最终的用户态 RSP，这个值会被放入 iretq 框架的 rsp 字段
	return current_user_vaddr;

fail_args:
	kfree(arg_vaddrs);

fail:
	vmm_unmap((uint64_t*)phys_to_virt(pml4_phys), user_stack_top - PAGE_SIZE);
	pmm_free((void*)stack_page_phys);
	if (phys_out) {
		*phys_out = 0;
	}

	return 0;
}

/** elf_section() - 按索引直接拿 section header */
inline Elf64_Shdr *elf_section(Elf64_Ehdr *ehdr, uint16_t idx)
{
	return (Elf64_Shdr*)((uint8_t*)ehdr + ehdr->e_shoff)  + idx;
}
