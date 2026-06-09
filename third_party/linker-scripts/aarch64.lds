/*
 * aarch64.lds — ARM64 链接脚本
 *
 * QEMU -M virt 直接将 kernel ELF 加载到 RAM 基址 0x40000000。
 */

OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
	limine_requests PT_LOAD FLAGS(4); /* Limine protocol metadata */
	text    PT_LOAD FLAGS(5);   /* R-X */
	rodata  PT_LOAD FLAGS(4);   /* R-- */
	data    PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
	. = 0x40000000;

	/* Keep protocol requests in the loaded image even with --gc-sections. */
	.limine_requests : {
		KEEP(*(.limine_requests_start))
		KEEP(*(.limine_requests))
		KEEP(*(.limine_requests_end))
	} :limine_requests

	. = ALIGN(4096);

	.text : {
		*(.text.boot)    /* startup.S 放最前面 */
		*(.text .text.*)
	} :text

	. = ALIGN(4096);

	.rodata : {
		*(.rodata .rodata.*)
	} :rodata

	. = ALIGN(4096);

	.data : {
		*(.data .data.*)
	} :data

	.bss : {
		bss_begin = .;
		*(.bss .bss.*)
		*(COMMON)
		bss_end = .;
	} :data

	/* kallsyms 符号表 */
	__ksymtab : {
		__start___ksymtab = .;
		KEEP(*(__ksymtab))
		__stop___ksymtab = .;
	}

		__ksymtab_strings : {
			__start___ksymtab_strings = .;
			*(__ksymtab_strings)
			__stop___ksymtab_strings = .;
		}

		/* 内核映像结束（链接器导出，用于 PMM 避开内核占用页） */
		kernel_end = ALIGN(4096);

		/DISCARD/ : {
		*(.eh_frame*)
		*(.note .note.*)
	}
}
