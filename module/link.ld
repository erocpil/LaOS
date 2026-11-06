ENTRY(_start)

SECTIONS
{
    /* 这里的地址必须和 elf_load_segment 映射的地址空间一致 */
    /* . = 0x400000; */
	. = 0xffffffff80100000;

    .text : {
        *(.text .text.*)
    }

    .rodata ALIGN(0x1000) : {
        *(.rodata .rodata.*)
    }

    /* 注入器按段名定位元数据；ET_EXEC 中的指针由链接器解析。 */
    __laos_params ALIGN(0x1000) : {
        KEEP(*(__laos_params))
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
    }
}
