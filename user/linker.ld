/* ALIGN(4K): 非常关键。当在内核中使用 load_elf 时，磁盘上的偏移和内存中的虚拟地址如果都是 4K 对齐的，
 * 可以直接把文件的一整块拷贝到物理页。如果不对齐，可能需要处理一个物理页里包含两个不同权限段（比如一部分代码，
 * 一部分数据）的复杂情况。
 * PT_LOAD 段生成： 只要指定这些基本的段，ld 链接器会自动生成两个或更多的 PT_LOAD 程序头。
 *   一个包含 .text 和 .rodata（只读权限）。
 *   一个包含 .data 和 .bss（读写权限）。
 * .bss 段： 注意 .bss 段在 ELF 文件中不占字节，但在 Elf64_Phdr 中，它的 p_memsz 会大于 p_filesz。
 * 这意味着加载器必须把多出来的这部分内存手动清零。
 */

/* 设置程序入口点，必须与 crt0.s 中的符号一致 */
ENTRY(_start)

SECTIONS
{
	/* 用户态程序通常起始于 0x400000 */
	. = 0x400000;

	/* 代码段：只读，可执行 */
	.text : ALIGN(4K)
	{
		*(.text .text.*)
}

/* 只读数据段 */
.rodata : ALIGN(4K)
{
	*(.rodata .rodata.*)
}

/* 已初始化数据段：可读，可写 */
.data : ALIGN(4K)
{
	*(.data .data.*)
}

/* 未初始化数据段 (BSS)：在文件中不占用空间，在内存中清零 */
.bss : ALIGN(4K)
{
	*(COMMON)
		*(.bss .bss.*)
}

/* 丢弃不需要的段，减小 ELF 文件体积 */
/DISCARD/ :
{
	*(.comment)
		*(.eh_frame)
		*(.note.*)
}
}
