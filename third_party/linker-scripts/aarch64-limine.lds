/* AArch64 Limine image layout.  This script is intentionally separate from
 * aarch64.lds: the latter is the low-address QEMU -kernel image. */
OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(limine_entry)

PHDRS {
    limine_requests PT_LOAD FLAGS(4);
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS {
    . = 0xffffffff80000000;

    .limine_requests : ALIGN(8) {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .text : { *(.text.boot.limine) *(.text .text.*) } :text
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    /* Keep all post-text sections in one non-executable segment.  This avoids
     * GNU ld extending a read-only PHDR over the following NOBITS bss area. */
    .rodata : { *(.rodata .rodata.*) } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : { *(.data .data.*) } :data
    .bss : {
        bss_begin = .;
        *(.bss .bss.*) *(COMMON)
        bss_end = .;
    } :data

    /* Keep generated symbol metadata in the writable segment.  Placing it
     * after bss avoids GNU ld extending the read-only PHDR across bss. */
    __ksymtab : {
        __start___ksymtab = .; KEEP(*(__ksymtab)); __stop___ksymtab = .;
    } :data
    __ksymtab_strings : {
        __start___ksymtab_strings = .; *(__ksymtab_strings);
        __stop___ksymtab_strings = .;
    } :data

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) }
}
