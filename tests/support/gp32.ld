/* Links a test program as a GP32 executable image loaded at 0x0C000000. */
ENTRY(_start)
SECTIONS
{
    . = 0x0C000000;
    __text_start = .;
    .text : { KEEP(*(.init)) *(.text .text.* .glue_7 .glue_7t) }
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4);
    __text_end = .;
    __data_start = .;
    .data : { *(.data .data.*) }
    . = ALIGN(4);
    __data_end = .;
    __bss_start = .;
    .bss : { *(.bss .bss.* COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note*) }
}
