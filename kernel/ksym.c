/*
 * ksym.c - 内核符号表(ksymtab)注册与查找
 *
 * 模块在载入时通过 EXPORT_SYMBOL 向全局符号表注册导出符号。
 * ksym_lookup 运行期按名字线性查表，双向解析(kernel <-> module)。
 */
#include <stdint.h>

#include "elf.h"
#include "string.h"
#include "export.h"
#include "debug.h"

extern const struct kernel_symbol __start___ksymtab[];
extern const struct kernel_symbol __stop___ksymtab[];

/* 完整符号表(由 gen_kallsyms.sh 从 nm 输出生成，包含所有函数) */
extern const struct {
	uint64_t addr;
	const char *name;
} kallsyms_all[];
extern const int KALLSYMS_COUNT;

void ksym_dump_all(void)
{
	extern const struct kernel_symbol __start___ksymtab[];
	extern const struct kernel_symbol __stop___ksymtab[];

	const struct kernel_symbol *sym = __start___ksymtab;
	int count = 0;

	L("[ksym] __start___ksymtab addr = %p", (void*)__start___ksymtab);
	L("[ksym] __stop___ksymtab  addr = %p", (void*)__stop___ksymtab);

	L("[ksym] table range: %p ~ %p", __start___ksymtab, __stop___ksymtab);

	while (sym < __stop___ksymtab) {
		L("[ksym] [%3d] addr=0x%016lx  name=%s", count, sym->addr, sym->name);
		sym++;
		count++;
	}

	L("[ksym] total: %d symbols.", count);
}

uint64_t ksym_lookup(const char *name)
{
	const struct kernel_symbol *sym;

	for (sym = __start___ksymtab; sym < __stop___ksymtab; sym++) {
		if (strcmp(sym->name, name) == 0) {
			return sym->addr;
		}
	}

	return 0;
}

const char *kallsyms_lookup(uint64_t ret_addr, uint64_t *offset)
{
	const struct kernel_symbol *sym;
	const struct kernel_symbol *best = NULL;
	uint64_t best_addr = 0;

	/* Pass 1: 搜索 EXPORT_SYMBOL 表(快速，覆盖导出函数) */
	for (sym = __start___ksymtab; sym < __stop___ksymtab; sym++) {
		if (sym->addr <= ret_addr && sym->addr > best_addr) {
			best = sym;
			best_addr = sym->addr;
		}
	}

	/* Pass 2: 搜索完整符号表(覆盖所有函数，包括未导出的) */
	const char *best_name = NULL;
	int from_kallsyms = 0;

	for (int i = 0; i < KALLSYMS_COUNT; i++) {
		uint64_t a = kallsyms_all[i].addr;
		if (a <= ret_addr && a > best_addr) {
			best_name = kallsyms_all[i].name;
			best_addr = a;
			from_kallsyms = 1;
		}
	}

	if (from_kallsyms) {
		*offset = ret_addr - best_addr;
		return best_name;
	}

	if (best && offset) {
		*offset = ret_addr - best->addr;
	}

	return best ? best->name : NULL;
}

/** 从 ELF 中找到 section header(按名字) */
Elf64_Shdr *find_section(Elf64_Ehdr *ehdr, const char *name)
{
	/* section name string table */
	Elf64_Shdr *shstr_hdr = (Elf64_Shdr*)((uint8_t*)ehdr
			+ ehdr->e_shoff + ehdr->e_shstrndx * ehdr->e_shentsize);
	const char *shstrtab = (const char*)((uint8_t*)ehdr
			+ shstr_hdr->sh_offset);

	Elf64_Shdr *shdr = (Elf64_Shdr*)((uint8_t*)ehdr + ehdr->e_shoff);
	for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
		if (strcmp(shstrtab + shdr[i].sh_name, name) == 0) {
			return &shdr[i];
		}
	}

	return NULL;
}

/** 根据 rela 条目的符号索引，找到符号名字符串 */
const char *get_sym_name(Elf64_Ehdr *ehdr, Elf64_Rela *rela_entry)
{
	/*
	 * 每个 .rela.* section 都有一个关联的符号表，
	 * 通过 .rela section 的 sh_link 字段指向对应的 .symtab section.
	 */
	Elf64_Shdr *shdr = (Elf64_Shdr*)((uint8_t*)ehdr + ehdr->e_shoff);

	/* 找到 .rela.text section，从它的 sh_link 拿到符号表索引 */
	Elf64_Shdr *rela_shdr = find_section(ehdr, ".rela.text");
	if (!rela_shdr) {
		return NULL;
	}

	Elf64_Shdr *symtab_shdr = &shdr[rela_shdr->sh_link];
	Elf64_Sym *symtab = (Elf64_Sym*)((uint8_t*)ehdr + symtab_shdr->sh_offset);

	/* symtab section 关联的字符串表通过 sh_link 指向 */
	Elf64_Shdr *strtab_shdr = &shdr[symtab_shdr->sh_link];
	const char *strtab = (const char*)((uint8_t*)ehdr + strtab_shdr->sh_offset);

	/* r_info 高 32 位是符号表索引 */
	uint32_t sym_idx = ELF64_R_SYM(rela_entry->r_info);

	return strtab + symtab[sym_idx].st_name;
}

/* 根据符号索引，直接返回 Elf64_Sym*(后面 apply 需要 st_value) */
Elf64_Sym *get_sym(Elf64_Ehdr *ehdr, Elf64_Rela *rela_entry)
{
	Elf64_Shdr *shdr = (Elf64_Shdr*)((uint8_t*)ehdr + ehdr->e_shoff);
	Elf64_Shdr *rela_shdr = find_section(ehdr, ".rela.text");
	Elf64_Shdr *symtab_shdr = &shdr[rela_shdr->sh_link];
	Elf64_Sym *symtab = (Elf64_Sym*)((uint8_t*)ehdr + symtab_shdr->sh_offset);
	uint32_t sym_idx = ELF64_R_SYM(rela_entry->r_info);

	return &symtab[sym_idx];
}

int apply_relocation_to(uint8_t *mod_base, uint64_t target_sh_offset,
		Elf64_Rela *rela_entry, uint64_t sym_addr)
{
	uint8_t *patch_addr = mod_base
		+ target_sh_offset
		+ rela_entry->r_offset;
	uint64_t P = (uint64_t)patch_addr;
	uint64_t S = sym_addr;
	int64_t A = rela_entry->r_addend;

	uint32_t r_type = ELF64_R_TYPE(rela_entry->r_info);
	uint32_t insn;
	int64_t delta;

	switch (r_type) {
	case R_X86_64_64: {
		uint64_t val = (uint64_t)(S + A);
		memcpy(patch_addr, &val, 8);
		break;
	}
	case R_X86_64_32: {
		uint64_t val = (uint64_t)(S + A);
		if (val >> 32) {
			L("ERROR: R_X86_64_32");
			return -1;
		}
		uint32_t v32 = (uint32_t)val;
		memcpy(patch_addr, &v32, 4);
		break;
	}
	case R_X86_64_32S: {
		int64_t val = (int64_t)(S + A);
		if (val != (int64_t)(int32_t)val) {
			L("ERROR: R_X86_64_32S");
			return -1;
		}
		int32_t v32 = (int32_t)val;
		memcpy(patch_addr, &v32, 4);
		break;
	}
	case R_X86_64_PC32:
	case R_X86_64_PLT32: {
		int64_t val = (int64_t)(S + A - P);
		if (val != (int64_t)(int32_t)val) {
			L("ERROR: R_X86_64_PC32/R_X86_64_PLT32");
			return -1;
		}
		int32_t v32 = (int32_t)val;
		memcpy(patch_addr, &v32, 4);
		break;
	}
	case R_X86_64_PC64: {
		int64_t val = (int64_t)(S + A - P);
		memcpy(patch_addr, &val, 8);
		break;
	}
	case R_X86_64_NONE:
		/* R_AARCH64_NONE is also 0 — same case, no conflict */
		break;
	case R_AARCH64_ABS64: {
		uint64_t val = (uint64_t)(S + A);
		memcpy(patch_addr, &val, 8);
		break;
	}
	case R_AARCH64_ABS32: {
		uint64_t val = (uint64_t)(S + A);
		if (val > UINT32_MAX) {
			L("ERROR: R_AARCH64_ABS32 overflow");
			return -1;
		}
		uint32_t v32 = (uint32_t)val;
		memcpy(patch_addr, &v32, sizeof(v32));
		break;
	}
	case R_AARCH64_PREL32: {
		delta = (int64_t)(S + A - P);
		if (delta != (int64_t)(int32_t)delta) {
			L("ERROR: R_AARCH64_PREL32 overflow");
			return -1;
		}
		int32_t v32 = (int32_t)delta;
		memcpy(patch_addr, &v32, sizeof(v32));
		break;
	}
	case R_AARCH64_CALL26:
	case R_AARCH64_JUMP26:
		delta = (int64_t)(S + A - P);
		if ((delta & 3) || delta < -(1LL << 27) ||
		    delta >= (1LL << 27)) {
			L("ERROR: AArch64 branch26 out of range P=%p S=%p",
				(void *)P, (void *)(S + A));
			return -1;
		}
		memcpy(&insn, patch_addr, sizeof(insn));
		insn = (insn & 0xfc000000U) |
			((uint32_t)(delta >> 2) & 0x03ffffffU);
		memcpy(patch_addr, &insn, sizeof(insn));
		break;
	case R_AARCH64_CONDBR19:
		delta = (int64_t)(S + A - P);
		if ((delta & 3) || delta < -(1LL << 20) ||
		    delta >= (1LL << 20)) {
			L("ERROR: AArch64 condbr19 out of range");
			return -1;
		}
		memcpy(&insn, patch_addr, sizeof(insn));
		insn = (insn & ~(0x7ffffU << 5)) |
			(((uint32_t)(delta >> 2) & 0x7ffffU) << 5);
		memcpy(patch_addr, &insn, sizeof(insn));
		break;
	case R_AARCH64_ADR_PREL_PG_HI21: {
		int64_t page_delta = ((int64_t)(S + A) & ~0xfffLL) -
			((int64_t)P & ~0xfffLL);
		int64_t imm = page_delta >> 12;
		if ((page_delta & 0xfff) || imm < -(1LL << 20) ||
		    imm >= (1LL << 20)) {
			L("ERROR: AArch64 ADRP out of range");
			return -1;
		}
		memcpy(&insn, patch_addr, sizeof(insn));
		insn &= ~((3U << 29) | (0x7ffffU << 5));
		insn |= ((uint32_t)imm & 3U) << 29;
		insn |= (((uint32_t)imm >> 2) & 0x7ffffU) << 5;
		memcpy(patch_addr, &insn, sizeof(insn));
		break;
	}
	case R_AARCH64_ADD_ABS_LO12_NC:
		memcpy(&insn, patch_addr, sizeof(insn));
		insn = (insn & ~(0xfffU << 10)) |
			(((uint32_t)(S + A) & 0xfffU) << 10);
		memcpy(patch_addr, &insn, sizeof(insn));
		break;
	case R_AARCH64_LDST8_ABS_LO12_NC:
	case R_AARCH64_LDST32_ABS_LO12_NC:
	case R_AARCH64_LDST64_ABS_LO12_NC: {
		unsigned int scale = r_type == R_AARCH64_LDST8_ABS_LO12_NC ? 0 :
			(r_type == R_AARCH64_LDST32_ABS_LO12_NC ? 2 : 3);
		uint64_t low = (S + A) & 0xfff;
		if (low & ((1U << scale) - 1)) {
			L("ERROR: AArch64 LDST LO12 alignment");
			return -1;
		}
		memcpy(&insn, patch_addr, sizeof(insn));
		insn = (insn & ~(0xfffU << 10)) |
			((uint32_t)(low >> scale) << 10);
		memcpy(patch_addr, &insn, sizeof(insn));
		break;
	}
	case R_AARCH64_RELATIVE: {
		/* Delta(S) + A: Delta(S) = runtime base of shared object.
		 * For ET_EXEC static binaries, base = 0, so result = S + A.
		 * For ET_REL modules loaded at arbitrary VA, base is relocatable.
		 * Current implementation: treat as S + A (works for ET_EXEC). */
		uint64_t val = (uint64_t)(S + A);
		memcpy(patch_addr, &val, 8);
		break;
	}
	default:
		L("ERROR: unknown r_type %d", r_type);
		return -1;
	}

	return 0;
}

/** 在模块自己的符号表里按名字查运行时地址 */
uintptr_t ksym_in_module(Elf64_Ehdr *ehdr, uint8_t *mem, const char *name)
{
	Elf64_Shdr *symtab_shdr = find_section(ehdr, ".symtab");

	if (!symtab_shdr) {
		return 0;
	}

	Elf64_Shdr *strtab_shdr = elf_section(ehdr, symtab_shdr->sh_link);
	const char *strtab = (const char*)(mem + strtab_shdr->sh_offset);
	Elf64_Sym *symtab = (Elf64_Sym*)(mem + symtab_shdr->sh_offset);
	uint64_t n = symtab_shdr->sh_size / sizeof(Elf64_Sym);

	for (uint64_t i = 0; i < n; i++) {
		if (strcmp(strtab + symtab[i].st_name, name) == 0) {
			Elf64_Shdr *sec = elf_section(ehdr, symtab[i].st_shndx);
			return (uintptr_t)(mem + sec->sh_offset + symtab[i].st_value);
		}
	}

	return 0;
}
