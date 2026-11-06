/* 在设备初始化和并发任务启动前，同步检查 ELF 加载器的资源所有权与回滚行为。 */
#include "tests/test_user_loader.h"
#include "elf.h"
#include "thread.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "hhdm.h"
#include "arch_tlb.h"
#include "string.h"
#include "printf.h"

static uint64_t live_heap_bytes(void)
{
	uint64_t bytes = 0;
	struct kheap_node *node;
	list_for_each_entry(node, &kheap_g.head.node, node) {
		if (!node->is_free) {
			bytes += node->size;
		}
	}
	return bytes;
}

static bool load_and_discard(uint8_t *image, int size, bool fail_stack)
{
	char *args[] = {"loader-fixture", NULL};
	uint64_t root = arch_read_cr3();
	struct thread *thread = create_elf_process(image, size, fail_stack ? 0 : 1, args);
	if (fail_stack) {
		if (thread) {
			thread_destroy(thread);
			return false;
		}
	} else {
		if (!thread) {
			return false;
		}
		bool private = thread->pml4_phys &&
			       (uint64_t)thread->pml4_phys != virt_to_phys(kernel_pml4);
		thread_destroy(thread); /* 该线程尚未加入任何运行队列。 */
		if (!private) {
			return false;
		}
	}
	return arch_read_cr3() == root;
}

bool test_user_loader_run(void)
{
	struct {
		Elf64_Ehdr header;
		Elf64_Phdr segment;
		uint8_t code[8];
	} image = {0};
	memcpy(image.header.e_ident, "\177ELF\2\1\1", 7);
	image.header.e_type = ET_EXEC;
	image.header.e_machine = EM_X86_64;
	image.header.e_ehsize = sizeof(Elf64_Ehdr);
	image.header.e_phentsize = sizeof(Elf64_Phdr);
	image.header.e_phoff = sizeof(Elf64_Ehdr);
	image.header.e_phnum = 1;
	image.header.e_entry = 0x400000;
	image.segment.p_type = PT_LOAD;
	image.segment.p_flags = PF_R | PF_X;
	image.segment.p_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
	image.segment.p_vaddr = image.header.e_entry;
	image.segment.p_filesz = sizeof(image.code);
	image.segment.p_memsz = sizeof(image.code);
	uint8_t *bytes = (uint8_t *)&image;
	/* 先触发堆扩展；后续加载和回滚应恢复页数及已分配堆块数量。 */
	if (!load_and_discard(bytes, sizeof(image), false) ||
		!load_and_discard(bytes, sizeof(image), true)) {
		return false;
	}
	uint64_t pages = pinfo.page.freed, heap = live_heap_bytes();
	for (int i = 0; i < 4; i++) {
		if (!load_and_discard(bytes, sizeof(image), true) ||
			!load_and_discard(bytes, sizeof(image), false) ||
			pinfo.page.freed != pages || live_heap_bytes() != heap) {
			return false;
		}
	}
	if (create_elf_process(bytes, sizeof(Elf64_Ehdr), 1, NULL)) {
		return false;
	}
	kprintf("[test_user_loader] PASSED: private roots, CR3 restore, stack-failure rollback, "
		"page/heap balance\n");
	return true;
}
