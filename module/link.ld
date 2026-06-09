ENTRY(_start)

SECTIONS
{
    /* 这里的地址必须和 elf_load_segment 映射的地址空间一致 */
    /* . = 0x400000; */
	. = 0xffffffff80100000;

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
        *(__laos_params)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
    }
}
