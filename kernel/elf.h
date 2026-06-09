#ifndef __ELF_H__
#define __ELF_H__

/*
 * elf.h - ELF 文件格式定义与加载接口
 */

#include <stdint.h>
#include <stdbool.h>

/* ELF 基本类型定义 (x86_64) */
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

/* ELF Header (Ehdr) - 文件头 */
#define EI_NIDENT 16

/* e_ident indexes and values required by this x86_64 loader. */
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT  1

/* ELF machine identifiers */
#define EM_X86_64   62
#define EM_AARCH64 183

typedef struct {
	// 魔数和其他标识
	unsigned char e_ident[EI_NIDENT];
	// 1=Relocatable, 2=Executable, 3=Shared
	Elf64_Half e_type;
	// x86_64 为 62 (0x3E)
	Elf64_Half e_machine;
	Elf64_Word e_version;
	// 程序入口虚拟地址
	Elf64_Addr e_entry;
	// 程序头表偏移
	Elf64_Off e_phoff;
	// 节头表偏移
	Elf64_Off e_shoff;
	Elf64_Word e_flags;
	Elf64_Half e_ehsize;
	Elf64_Half e_phentsize;
	// 程序头表中的条目数
	Elf64_Half e_phnum;
	Elf64_Half e_shentsize;
	Elf64_Half e_shnum;
	Elf64_Half e_shstrndx;
} Elf64_Ehdr;

/* Program Header (Phdr) - 程序头/段头 */
typedef struct {
	// 1 = PT_LOAD (可加载段)
	Elf64_Word  p_type;
	// 1=X, 2=W, 4=R
	Elf64_Word  p_flags;
	// 段在文件中的偏移
	Elf64_Off   p_offset;
	// 段在内存中的虚拟地址
	Elf64_Addr  p_vaddr;
	// 物理地址 (通常忽略)
	Elf64_Addr  p_paddr;
	// 段在文件中的长度
	Elf64_Xword p_filesz;
	// 段在内存中的长度 (大于 filesz 则说明有 BSS)
	Elf64_Xword p_memsz;
	// 对齐
	Elf64_Xword p_align;
} Elf64_Phdr;

/* Section Header: 节头表条目 */
typedef struct {
	// 节名在 shstrtab 中的偏移
	Elf64_Word  sh_name;
	// 节类型
	Elf64_Word  sh_type;
	Elf64_Xword sh_flags;
	// 加载地址(ET_REL 中为 0)
	Elf64_Addr  sh_addr;
	// 节在文件中的偏移
	Elf64_Off   sh_offset;
	// 节的字节数
	Elf64_Xword sh_size;
	// 关联节的索引(如符号表->字符串表)
	Elf64_Word  sh_link;
	Elf64_Word  sh_info;
	Elf64_Xword sh_addralign;
	// 固定大小条目的大小(如 symtab)
	Elf64_Xword sh_entsize;
} Elf64_Shdr;

/* sh_type 常用值 */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8 /* BSS */
#define SHT_REL      9

/* 特殊节索引 */
#define SHN_UNDEF    0 /* 未定义符号 */
#define SHN_ABS      0xfff1 /* 绝对值符号，不需要重定位 */
#define SHN_COMMON   0xfff2

/* Symbol Table Entry: 符号表条目 */
typedef struct {
	// 符号名在 strtab 中的偏移
	Elf64_Word  st_name;
	// 类型和绑定属性
	unsigned char st_info;
	unsigned char st_other;
	// 所在节的索引，或 SHN_UNDEF/SHN_ABS
	Elf64_Half  st_shndx;
	// 符号值(ET_REL 中是节内偏移)
	Elf64_Addr  st_value;
	Elf64_Xword st_size;
} Elf64_Sym;

/* st_info 解析宏 */
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)

/* 绑定类型 */
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

/* 符号类型 */
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3

/* Relocation Entry: 重定位条目(带 addend) */
typedef struct {
	// 重定位槽在节中的偏移
	Elf64_Addr  r_offset;
	// 高32位：符号索引；低32位：重定位类型
	Elf64_Xword r_info;
	// 加数
	Elf64_Sxword r_addend;
} Elf64_Rela;

/* r_info 解析宏 */
#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xffffffffUL)
#define ELF64_R_INFO(s,t) (((Elf64_Xword)(s) << 32) | (t))

/* x86-64 重定位类型 */
#define R_X86_64_NONE  0
#define R_X86_64_64    1  /* S + A，64位绝对地址 */
#define R_X86_64_PC32  2  /* S + A - P，32位PC相对 */
#define R_X86_64_32    10 /* S + A，零扩展到64位 */
#define R_X86_64_32S   11 /* S + A，符号扩展到64位 */
#define R_X86_64_PC64  24 /* S + A - P，64位PC相对 */
#define R_X86_64_PLT32 4  /* 与PC32相同，用于函数调用 */

/* AArch64 relocation types */
#define R_AARCH64_NONE               0
#define R_AARCH64_ABS64              257  /* S + A                          */
#define R_AARCH64_ABS32              258  /* S + A (truncated to 32-bit)    */
#define R_AARCH64_PREL32             261  /* S + A - P                      */
#define R_AARCH64_CALL26             283  /* S + A - P (B/BL, ±128MB)       */
#define R_AARCH64_JUMP26             282  /* S + A - P (B, ±128MB)          */
#define R_AARCH64_CONDBR19           280  /* S + A - P (B.cond, ±1MB)      */
#define R_AARCH64_ADR_PREL_PG_HI21   275  /* Page(S+A)-Page(P), ADRP instr */
#define R_AARCH64_ADD_ABS_LO12_NC    277  /* S + A, ADD immediate (no check)*/
#define R_AARCH64_LDST8_ABS_LO12_NC  278  /* S + A, LDR/STR imm (no check) */
#define R_AARCH64_LDST32_ABS_LO12_NC 285  /* S + A, scaled 32-bit access  */
#define R_AARCH64_LDST64_ABS_LO12_NC 286  /* S + A, LDR/STR 64-bit imm    */
#define R_AARCH64_RELATIVE           1027 /* Delta(S) + A (ET_EXEC dyn reloc) */

/* ELF 文件类型 */
#define ET_NONE 0
#define ET_REL  1 /* 可重定位(.ko) */
#define ET_EXEC 2 /* 可执行 */
#define ET_DYN  3 /* 共享库 */

/* 常用宏定义 */
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

/* 只有这种类型的段需要被加载到内存 */
#define PT_LOAD 1

/* 段权限标志 */
#define PF_X 1 /* 执行 */
#define PF_W 2 /* 写入 */
#define PF_R 4 /* 读取 */

#define USER_STACK_TOP 0x7FFFFFFFF000

int elf_check(Elf64_Ehdr *ehdr);
bool elf_load_segment(uint64_t *pml4_phys, Elf64_Phdr *phdr, uint8_t *elf_raw, int is_user);
uint64_t setup_user_stack(uint64_t pml4_phys, int argc, char **argv, uint64_t *phys_out);
Elf64_Shdr *elf_section(Elf64_Ehdr *ehdr, uint16_t idx);

#endif
