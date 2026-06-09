/* ELF 加载：内核态可执行 / 可重定位 (.mo) / 用户态进程。
 * 入口跳板 arch_user_thread_entry_stub 用于用户线程从内核栈跳进用户态。
 */
#include "cpu.h"
#include "vmm.h"
#include "elf.h"
#include "heap.h"
#include "arch_tlb.h"
#include "string.h"
#include "printf.h"
#include "thread.h"
#include "ksym.h"
#include "debug.h"
#include "module_alloc.h"
#include "module_param.h"
#include "module.h"
#include "user_vmm.h"

static struct thread *kthread_load_elf_exec(uint8_t *elf_raw, int elf_size, void *data)
{
	(void)elf_size;

	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)elf_raw;

	/* 基本合法性检查 */
	if (-1 == elf_check(ehdr)) {
		return NULL;
	}

	uint64_t flags = save_and_disable_interrupts();

	/* ET_EXEC 已链接完成，elf_load_segment 将段数据拷贝到映射页。
	 * mem 仅为临时 ELF 缓冲区，用 kmalloc 分配，线程退出时由
	 * thread_destroy 通过 elf_load_addr 释放。 */
	uint8_t *mem = kmalloc(elf_size);
	if (!mem) {
		kprintf("[load] kmalloc(elf) failed\n");
		/* P2-5: 入口处 save_and_disable_interrupts() 已关中断，
		 * 失败路径必须恢复，否则中断永久关闭导致系统僵死。 */
		restore_interrupts(flags);
		return NULL;
	}
	memcpy(mem, elf_raw, elf_size);

	ehdr = (Elf64_Ehdr*)mem;

	// 1. 加载段 (到当前内核页表)
	Elf64_Phdr *phdrs = (Elf64_Phdr*)(mem + ehdr->e_phoff);
	uint64_t current_pml4_phys = arch_read_cr3() & ~0xFFFULL;

	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type == PT_LOAD) {
			if (!elf_load_segment((uint64_t*)current_pml4_phys, &phdrs[i], mem, 0)) {
				kprintf("[load] elf_load_segment failed\n");
				kfree(mem);
				restore_interrupts(flags);
				return NULL;
			}
		}
	}

	// 2. 利用现有基础设施创建线程
	struct thread *t = thread_create((void (*)(void *))ehdr->e_entry, data);
	if (t) {
		t->id = THREAD_SET_KERNEL_PID();
		t->elf_load_addr = mem; /* thread_destroy 负责 kfree */
		t->elf_size = elf_size;
	}

	restore_interrupts(flags);

	return t;
}

#define MODULE_MAX_BSS_SECTIONS 8

struct module_bss_map {
	uint16_t sh_idx;
	uint64_t addr;
};

/**
 * module_alloc_bss() - 给 SHT_NOBITS (BSS/COMMON) section 分配独立内存。
 *
 * ET_REL 中 NOBITS section 的 sh_offset 经常与 PROGBITS section 重叠
 * (例： .bss sh_offset == .rodata.str* sh_offset).
 * 必须为每个 NOBITS section 单独分配清零的物理页，否则 BSS 全局/static
 * 变量的运行时地址会落到 .rodata 字符串区，读写互相覆盖。
 *
 * 使用 module_alloc 而非 pmm_alloc_pages+PHYS_TO_VIRT:BSS 引用
 * 在模块 .text 中通常以 R_X86_64_PC32 / R_X86_64_32S 形式出现，
 * 要求目标地址距模块代码段 +/-2GB.HHDM (0xffff8000_xxxx) 距
 * MODULE_VBASE (0xffffffffc0_xxxx) 约 2TB，PC32 立即越界。
 * module_alloc 在 -2GB 区段内 bump，与模块代码同距离 kernel，
 * 才能让 BSS reloc 通过。
 * 返回分配的 NOBITS section 数；-1 表示失败。
 */
static int module_alloc_bss(Elf64_Ehdr *ehdr, uint8_t *mem,
		struct module_bss_map *bss_map, int cap)
{
	(void)mem;

	int bss_count = 0;
	Elf64_Shdr *shdr_arr = (Elf64_Shdr*)((uint8_t*)ehdr + ehdr->e_shoff);

	for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
		if (shdr_arr[i].sh_type != SHT_NOBITS) {
			continue;
		}
		if (!(shdr_arr[i].sh_flags & 0x2)) {
			continue; /* SHF_ALLOC */
		}
		if (shdr_arr[i].sh_size == 0) {
			continue;
		}
		if (bss_count >= cap) {
			kprintf("[load] too many bss sections\n");
			return -1;
		}
		uint64_t size = (shdr_arr[i].sh_size + 4095) & ~4095UL;
		void *vaddr = module_alloc(size);
		if (!vaddr) {
			kprintf("[load] bss module_alloc fail (size=%lu)\n", size);
			return -1;
		}
		memset(vaddr, 0, size);
		bss_map[bss_count].sh_idx = i;
		bss_map[bss_count].addr = (uint64_t)vaddr;
		bss_count++;
		L("[load] bss sec[%u] size=%lu addr=%p", i, shdr_arr[i].sh_size, vaddr);
	}

	return bss_count;
}

/** module_bss_lookup() - 在 bss_map 里查 sh_idx 对应的 BSS section 基址 */
static uint64_t module_bss_lookup(const struct module_bss_map *bss_map,
		int bss_count, uint16_t sh_idx)
{
	for (int j = 0; j < bss_count; j++) {
		if (bss_map[j].sh_idx == sh_idx) {
			return bss_map[j].addr;
		}
	}

	return 0;
}

static bool module_relocation_type_supported(uint32_t type)
{
#if defined(__x86_64__)
	return type == R_X86_64_NONE || type == R_X86_64_64 ||
		type == R_X86_64_PC32 || type == R_X86_64_PLT32 ||
		type == R_X86_64_32 || type == R_X86_64_32S ||
		type == R_X86_64_PC64;
#elif defined(__aarch64__)
	return type == R_AARCH64_NONE || type == R_AARCH64_ABS64 ||
		type == R_AARCH64_ABS32 || type == R_AARCH64_PREL32 ||
		type == R_AARCH64_CALL26 || type == R_AARCH64_JUMP26 ||
		type == R_AARCH64_CONDBR19 ||
		type == R_AARCH64_ADR_PREL_PG_HI21 ||
		type == R_AARCH64_ADD_ABS_LO12_NC ||
		type == R_AARCH64_LDST8_ABS_LO12_NC ||
		type == R_AARCH64_LDST32_ABS_LO12_NC ||
		type == R_AARCH64_LDST64_ABS_LO12_NC ||
		type == R_AARCH64_RELATIVE;
#else
	(void)type;
	return false;
#endif
}

/* Reject deterministic loader failures before consuming bump-allocated module
 * VA. Runtime sections only: DWARF relocations are intentionally ignored by
 * the loader and must not affect loadability. */
static int module_preflight_relocations(Elf64_Ehdr *ehdr)
{
	Elf64_Shdr *shdrs = (Elf64_Shdr*)((uint8_t*)ehdr + ehdr->e_shoff);

	for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
		Elf64_Shdr *rela = &shdrs[si];
		if (rela->sh_type != SHT_RELA) {
			continue;
		}
		Elf64_Shdr *target = &shdrs[rela->sh_info];
		if (!(target->sh_flags & 0x2)) {
			continue;
		}

		Elf64_Shdr *symtab_hdr = &shdrs[rela->sh_link];
		Elf64_Sym *symtab = (Elf64_Sym*)((uint8_t*)ehdr + symtab_hdr->sh_offset);
		Elf64_Shdr *strtab_hdr = &shdrs[symtab_hdr->sh_link];
		const char *strtab = (const char*)ehdr + strtab_hdr->sh_offset;
		Elf64_Rela *entries = (Elf64_Rela*)((uint8_t*)ehdr + rela->sh_offset);
		uint64_t count = rela->sh_size / sizeof(*entries);

		for (uint64_t i = 0; i < count; i++) {
			uint32_t type = ELF64_R_TYPE(entries[i].r_info);
			if (!module_relocation_type_supported(type)) {
				kprintf("[load] unsupported module relocation type %u\n", type);
				return -1;
			}
			Elf64_Sym *sym = &symtab[ELF64_R_SYM(entries[i].r_info)];
			if (sym->st_shndx == SHN_UNDEF) {
				const char *name = strtab + sym->st_name;
				if (!ksym_lookup(name)) {
					kprintf("[load] unresolved module symbol before allocation: %s\n", name);
					return -1;
				}
			}
		}
	}

	return 0;
}

/**
 * module_apply_rela() - 遍历所有 .rela.* 节（.rela.text, .rela.text.startup 等），
 *  对每条重定位条目做符号解析并应用到目标 section。
 *
 *  ELF 约定：.rela.* 节的 sh_info 指向被重定位的目标 section 索引。
 */
static int module_apply_rela(Elf64_Ehdr *ehdr, uint8_t *mem, uint8_t *elf_raw,
		const struct module_bss_map *bss_map, int bss_count)
{
	Elf64_Shdr *shdrs = (Elf64_Shdr*)((uint8_t*)ehdr + ehdr->e_shoff);

	for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
		Elf64_Shdr *rela_shdr = &shdrs[si];
		if (rela_shdr->sh_type != SHT_RELA) {
			continue;
		}

		/* 目标 section 索引，例如 .rela.text -> .text */
		Elf64_Shdr *target_shdr = &shdrs[rela_shdr->sh_info];
		/* Debug sections are not part of the runtime module image. */
		if (!(target_shdr->sh_flags & 0x2)) {
			continue;
		}
		/* 目标 section 名字（仅用于日志） */
		const char *shstrtab = (const char*)((uint8_t*)ehdr
				+ elf_section(ehdr, ehdr->e_shstrndx)->sh_offset);

		Elf64_Rela *entries = (Elf64_Rela*)(elf_raw + rela_shdr->sh_offset);
		uint64_t n = rela_shdr->sh_size / sizeof(Elf64_Rela);

		L("[load] processing %lu relocs in '%s' -> '%s'",
				n, shstrtab + rela_shdr->sh_name,
				shstrtab + target_shdr->sh_name);

		/* 每个 .rela.* 节的符号表通过 sh_link 索引 */
		Elf64_Shdr *symtab_shdr = &shdrs[rela_shdr->sh_link];
		Elf64_Sym *symtab = (Elf64_Sym*)((uint8_t*)ehdr + symtab_shdr->sh_offset);
		Elf64_Shdr *strtab_shdr = &shdrs[symtab_shdr->sh_link];
		const char *strtab = (const char*)((uint8_t*)ehdr + strtab_shdr->sh_offset);

		for (uint64_t i = 0; i < n; i++) {
			Elf64_Rela *entry = &entries[i];
			uint32_t sym_idx = ELF64_R_SYM(entry->r_info);
			Elf64_Sym *sym = &symtab[sym_idx];
			const char *sym_name = strtab + sym->st_name;

			if (!sym_name || !strlen(sym_name)) {
				L("xxx NULL '%s'", sym_name);
			} else {
				L("xxx lookup '%s'", sym_name);
			}

			uint64_t sym_addr = 0;
			if (sym->st_shndx == SHN_UNDEF) {
				/* 外部符号：从内核导出表查找 */
				sym_addr = ksym_lookup(sym_name);
				if (!sym_addr) {
					L("[load] symbol not found: '%s'", sym_name);
					return -1;
				}
				L("[load] symbol found: '%s'", sym_name);
			} else if (sym->st_shndx == SHN_ABS) {
				/* 绝对符号：直接用值 */
				sym_addr = (uint64_t)sym->st_value;
			} else {
				Elf64_Shdr *sym_sec = elf_section(ehdr, sym->st_shndx);
				if (sym_sec->sh_type == SHT_NOBITS) {
					uint64_t base = module_bss_lookup(bss_map, bss_count, sym->st_shndx);
					if (!base) {
						kprintf("[load] bss sym '%s' shndx=%u not mapped\n",
								sym_name, sym->st_shndx);
						return -1;
					}
					sym_addr = base + sym->st_value;
				} else {
					sym_addr = (uint64_t)(mem + sym_sec->sh_offset + sym->st_value);
				}
			}

			int ret = apply_relocation_to(mem, target_shdr->sh_offset,
					entry, sym_addr);
			if (ret < 0) {
				kprintf("[load] relocation failed at entry %lu in '%s'\n",
						i, shstrtab + rela_shdr->sh_name);
				return -1;
			}
		}
	}

	return 0;
}

/**
 * module_find_entry() - 解析模块入口。
 *
 * ET_REL 的 e_entry 通常为 0，按名字查 main 或 _start.
 */
static uintptr_t module_find_entry(Elf64_Ehdr *ehdr, uint8_t *mem)
{
	uintptr_t entry;

	entry = (uintptr_t)ksym_in_module(ehdr, mem, "main");
	if (!entry) {
		entry = (uintptr_t)ksym_in_module(ehdr, mem, "_start");
	}
	if (!entry) {
		kprintf("[load] main or _start not found in module\n");
		return 0;
	}
	L("[load] entry = %p", (void*)entry);

	return entry;
}

static struct thread *kthread_load_elf_rel(uint8_t *elf_raw, int elf_size,
		const char *name, void *data)
{
	Elf64_Ehdr *ehdr_raw = (Elf64_Ehdr*)elf_raw;
	if (-1 == elf_check(ehdr_raw)) {
		L();
		return NULL;
	}
	if (module_preflight_relocations(ehdr_raw) < 0)
		return NULL;

	uint64_t flags = save_and_disable_interrupts();
	struct module_alloc_checkpoint checkpoint = module_alloc_checkpoint();

	L("[load] cpu=%d elf_raw=%p size=%lu\n", cpu_get_ctx()->id, elf_raw, elf_size);

	/*
	 * 每次加载复制独立副本，重定位不污染原始数据。
	 * 用 module_alloc 而不是 kmalloc:ET_REL 模块代码段必须落在距
	 * kernel .text +-2GB 内才能让 R_X86_64_PC32 / PLT32 重定位通过。
	 */
	uint8_t *mem = module_alloc(elf_size);
	if (!mem) {
		kprintf("[load] module_alloc(elf) failed\n");
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return NULL;
	}
	memcpy(mem, elf_raw, elf_size);
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)mem;

	struct module_bss_map bss_map[MODULE_MAX_BSS_SECTIONS] = {0};
	int bss_count = module_alloc_bss(ehdr, mem, bss_map, MODULE_MAX_BSS_SECTIONS);
	if (bss_count < 0) {
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return NULL;
	}

	if (module_apply_rela(ehdr, mem, elf_raw, bss_map, bss_count) < 0) {
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return NULL;
	}

	arch_module_sync_icache(mem, elf_size);

	uintptr_t entry = module_find_entry(ehdr, mem);
	if (!entry) {
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return NULL;
	}

	/* Build descriptor for registry (no init side effects yet) */
	struct module_desc desc = {0};
	desc.kind = MODULE_KIND_KTHREAD;
	desc.base = mem;
	desc.size = elf_size;
	desc.entry = (void *)entry;
	desc.name = name;
	for (int i = 0; i < bss_count; i++) {
		desc.bss_segments[i].base = (void *)bss_map[i].addr;
		Elf64_Shdr *shdrs = (Elf64_Shdr *)((uint8_t *)ehdr + ehdr->e_shoff);
		desc.bss_segments[i].size = shdrs[bss_map[i].sh_idx].sh_size;
	}
	desc.bss_count = bss_count;

	int mod_id = module_registry_reserve(&desc);
	if (mod_id < 0) {
		kprintf("[load] registry reserve failed\n");
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return NULL;
	}

	struct thread *t = thread_create_common((void (*)(void*))entry, data);
	if (t) {
		t->id = THREAD_SET_KERNEL_PID();
		t->entry_point = entry; /* TCB 路径需要，与 R15 走私并行保存 */
		t->elf_load_addr = mem;
		t->elf_size = elf_size;
		module_registry_commit(mod_id);
	} else {
		module_registry_cancel(mod_id);
		module_alloc_rollback(checkpoint);
	}

	restore_interrupts(flags);

	return t;
}

/**
 * selftest_load_payload() - 同步加载 selftest 载荷模块。
 *
 * 加载 ET_REL .mo 模块（同 kthread_load_elf_rel 的加载+重定位+BSS 流程），
 * 但不创建线程——而是查找 "selftest_init" 符号并同步调用。
 * selftest_init() 内部应调用 selftest_register() 注册测试。
 *
 * 流程：加载+重定位 → 预留注册表槽位 → 调 selftest_init() → 提交槽位。
 * 使用 reserve/commit 确保 selftest_init 产生副作用前槽位已预留，
 * 失败路径通过 cancel 安全释放。
 *
 * 返回注册表 ID（>=0），失败返回 -1。
 * 调用时机：task_init() 之后、schedule() 之前。
 */
int selftest_load_payload(uint8_t *elf_raw, int elf_size,
		const char *module_name)
{
	Elf64_Ehdr *ehdr_raw = (Elf64_Ehdr*)elf_raw;
	if (-1 == elf_check(ehdr_raw)) {
		kprintf("[selftest] payload ELF check failed\n");
		return -1;
	}
	if (module_preflight_relocations(ehdr_raw) < 0)
		return -1;

	uint64_t flags = save_and_disable_interrupts();
	struct module_alloc_checkpoint checkpoint = module_alloc_checkpoint();

	uint8_t *mem = module_alloc(elf_size);
	if (!mem) {
		kprintf("[selftest] payload module_alloc failed\n");
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return -1;
	}
	memcpy(mem, elf_raw, elf_size);
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)mem;

	struct module_bss_map bss_map[MODULE_MAX_BSS_SECTIONS] = {0};
	int bss_count = module_alloc_bss(ehdr, mem, bss_map, MODULE_MAX_BSS_SECTIONS);
	if (bss_count < 0) {
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return -1;
	}

	if (module_apply_rela(ehdr, mem, elf_raw, bss_map, bss_count) < 0) {
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return -1;
	}

	arch_module_sync_icache(mem, elf_size);

	/* Look up selftest_init before reserving registry slot */
	uintptr_t init_fn = (uintptr_t)ksym_in_module(ehdr, mem, "selftest_init");
	if (!init_fn) {
		kprintf("[selftest] payload missing selftest_init\n");
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return -1;
	}

	/* Build module_desc from load info (before init side effects) */
	struct module_desc desc = {0};
	desc.kind  = MODULE_KIND_SELFTEST;
	desc.base  = mem;
	desc.size  = elf_size;
	desc.init  = (void *)init_fn;
	desc.name  = module_name;
	for (int i = 0; i < bss_count; i++) {
		desc.bss_segments[i].base = (void *)bss_map[i].addr;
		size_t seg_size = 0;
		/* Get section size from ELF headers */
		Elf64_Shdr *shdr_arr = (Elf64_Shdr *)((uint8_t *)ehdr + ehdr->e_shoff);
		seg_size = shdr_arr[bss_map[i].sh_idx].sh_size;
		desc.bss_segments[i].size = seg_size;
	}
	desc.bss_count = bss_count;

	/* Reserve registry slot BEFORE calling selftest_init.
	 * If init produces side effects (selftest_register), the slot is
	 * already committed to the registry so we can safely roll back. */
	int mod_id = module_registry_reserve(&desc);
	if (mod_id < 0) {
		kprintf("[selftest] registry reserve failed\n");
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return -1;
	}

	kprintf("[selftest] calling selftest_init @ %p\n", (void*)init_fn);
	int init_ret = ((int (*)(void))init_fn)();
	if (init_ret < 0) {
		kprintf("[selftest] selftest_init failed (ret=%d)\n", init_ret);
		module_registry_cancel(mod_id);
		module_alloc_rollback(checkpoint);
		restore_interrupts(flags);
		return -1;
	}

	module_registry_commit(mod_id);
	restore_interrupts(flags);

	return mod_id;
}

/**
 * kthread_load_elf() - 内核线程 ELF 加载统一入口。
 *
 * 按 flags 路由到 EXEC / REL 加载器。
 */
struct thread *kthread_load_elf(uint8_t *elf_raw, int elf_size, int flags,
		const char *name, void *data)
{
	switch (flags) {
		case KTHREAD_ELF_EXEC:
			return kthread_load_elf_exec(elf_raw, elf_size, data);
		case KTHREAD_ELF_REL:
			return kthread_load_elf_rel(elf_raw, elf_size, name, data);
		default:
			kprintf("[kthread_load_elf] unknown flags=%d\n", flags);
			return NULL;
	}
}


struct thread *create_elf_process(uint8_t *elf_raw, int elf_size, int argc, void *argv)
{
	(void)elf_size;

	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)elf_raw;

	/* 基本合法性检查 */
	if (-1 == elf_check(ehdr)) {
		return NULL;
	}

	// 关闭本地中断并保存原中断状态
	uint64_t flags = save_and_disable_interrupts();

	/* 每次加载复制独立副本，重定位不污染原始数据 */
	uint8_t *mem = kmalloc(elf_size);
	if (!mem) {
		kprintf("[load] kmalloc(elf) failed\n");
		restore_interrupts(flags);
		return NULL;
	}

	memcpy(mem, elf_raw, elf_size);

	struct thread *t = NULL;

	t = (struct thread*)kmalloc(sizeof(struct thread));
	if (!t) {
		kfree(mem);
		restore_interrupts(flags);
		panic("create_elf_process: kmalloc(tcb) failed");
	}
	memset(t, 0, sizeof(struct thread));
	INIT_LIST_NODE(&t->node);
	INIT_LIST_NODE(&t->wait_node);
	thread_priority_init(t);

	// 创建私有页表
	t->pml4_phys = vmm_create_user_pml4();
	L("thread %p pml4_phys %p", t, t->pml4_phys);
	if (!t->pml4_phys || arch_user_vmm_init(t->pml4_phys) != 0) {
		kprintf("[load] initialize user address space failed\n");
		t->elf_load_addr = mem;
		t->elf_size = elf_size;
		thread_destroy(t);
		restore_interrupts(flags);
		return NULL;
	}

	// 加载 ELF 各个段
	ehdr = (Elf64_Ehdr*)mem;
	Elf64_Phdr *phdrs = (Elf64_Phdr*)(mem + ehdr->e_phoff);

	uint64_t cr3_token = arch_user_elf_load_begin((uint64_t)t->pml4_phys);

	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type == PT_LOAD) {
			if (!elf_load_segment(t->pml4_phys, &phdrs[i], mem, 1)) {
				kprintf("[load] elf_load_segment (user) failed\n");
				arch_user_elf_load_end(cr3_token);
				t->elf_load_addr = mem;
				t->elf_size = elf_size;
				thread_destroy(t);
				restore_interrupts(flags);
				return NULL;
			}
		}
	}

	// 准备入口点
	t->entry_point = ehdr->e_entry;

	// 准备栈和参数
	t->user_stack = (void*)setup_user_stack((uint64_t)t->pml4_phys, argc,
			(char**)argv, &t->user_stack_phys);

	arch_user_elf_load_end(cr3_token);
	if (!t->user_stack) {
		kprintf("[load] setup_user_stack failed\n");
		t->elf_load_addr = mem;
		t->elf_size = elf_size;
		thread_destroy(t);
		restore_interrupts(flags);
		return NULL;
	}

	t->is_user = true;
	t->id = THREAD_SET_USER_PID();
	ksprintf(t->name, "user%d", t->id);

	t->elf_load_addr = mem; /* thread_destroy 负责 kfree */
	t->elf_size = elf_size;

	t->kernel_stack_base = kmalloc(KERNEL_STACK_SIZE);
	if (!t->kernel_stack_base) {
		panic("Failed to allocate kernel stack\n");
	}

	// 栈顶(高地址)，对齐到 16 字节
	uint64_t stack_top = (uint64_t)t->kernel_stack_base + KERNEL_STACK_SIZE;
	stack_top &= ~0xFULL;
	t->kernel_stack = (void*)stack_top;
	t->rsp = arch_thread_init_user_frame(t, stack_top);

	/* 在所有线程创建时记录 */
	L("[stack_range] pid=%ld name=%s base=%p top=%p\n", t->id, t->name, t->kernel_stack_base, t->kernel_stack);

	restore_interrupts(flags);

	return t;
}

/**
 * module_apply_kv_params() - 将 task.conf 的 key=value 对写入模块变量。
 *
 * 遍历模块 ELF 的 __laos_params 段，匹配 kv 数组中的 key，
 * 将 value 解析后写入 MODULE_PARAM 声明的模块全局变量。
 * 模块 main() 执行时变量已是正确值，无需手动解析。
 */
void module_apply_kv_params(struct thread *th, int kv_count,
		char *kv_keys[], char *kv_values[])
{
	if (!kv_count || !th->elf_load_addr || !th->elf_size) {
		return;
	}

	Elf64_Ehdr *ehdr = (Elf64_Ehdr *)th->elf_load_addr;
	Elf64_Shdr *shdrs = (Elf64_Shdr *)((uint8_t *)ehdr + ehdr->e_shoff);
	const char *shstrtab = (const char *)((uint8_t *)ehdr
			+ shdrs[ehdr->e_shstrndx].sh_offset);

	for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
		const char *sec_name = shstrtab + shdrs[si].sh_name;
		if (strcmp(sec_name, "__laos_params") != 0) {
			continue;
		}

		struct laos_param *params = (struct laos_param *)
			((uint8_t *)ehdr + shdrs[si].sh_offset);
		uint64_t count = shdrs[si].sh_size / sizeof(struct laos_param);

		for (uint64_t i = 0; i < count; i++) {
			struct laos_param *p = &params[i];
			for (int k = 0; k < kv_count; k++) {
				if (strcmp(kv_keys[k], p->name) != 0) {
					continue;
				}
				switch (p->type) {
					case PARAM_INT: {
										int val = 0;
										for (char *c = kv_values[k]; *c; c++) {
											val = val * 10 + (*c - '0');
										}
										*(int *)p->ptr = val;
										break;
									}
					case PARAM_STRING: {
										   int len = strlen(kv_values[k]);
										   memcpy(p->ptr, kv_values[k], len + 1);
										   break;
									   }
					case PARAM_BOOL: {
										 char ch = kv_values[k][0];
										 int b = (ch == '1' || ch == 'y' || ch == 'Y');
										 *(int *)p->ptr = b;
										 break;
									 }
				}
				L("[param] %s=%s applied at %p",
						p->name, kv_values[k], p->ptr);
			}
		}
	}
}
