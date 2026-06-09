#ifndef __KSYM_H__
#define __KSYM_H__

/*
 * ksym.h - 内核符号表 __ksymtab
 */
#include "elf.h"

Elf64_Shdr *find_section(Elf64_Ehdr *ehdr, const char *name);
int apply_relocation_to(uint8_t *mod_base, uint64_t target_sh_offset,
		Elf64_Rela *rela_entry, uint64_t sym_addr);
uintptr_t ksym_in_module(Elf64_Ehdr *ehdr, uint8_t *mem, const char *name);
uint64_t ksym_lookup(const char *name);
const char *kallsyms_lookup(uint64_t ret_addr, uint64_t *offset);
const char *get_sym_name(Elf64_Ehdr *ehdr, Elf64_Rela *rela_entry);
void ksym_dump_all(void);

#endif
