/*
 * Core-B image. Core A copies it to 0x01000000 (the code RAM; core B sees it at
 * 0), so code and read-only data are linked at 0 with the vector table first.
 * Everything written lives in the data RAM (audio_shared.h): .data (loaded from
 * the copy behind .text by b_reset) and .bss from 0x01021000, then the decoder
 * heap, then the stack (B_STACK bytes) up to 0x01060000.
 */
ENTRY(b_reset)

MEMORY
{
    CODE (rx)  : ORIGIN = 0x00000000, LENGTH = 0x00020000   /* AS_B_IMAGE_MAX */
    DATA (rw)  : ORIGIN = 0x01021000, LENGTH = 0x0003F000   /* AS_B_DATA .. AS_B_TOP */
}

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > CODE
    .data : {
        _b_data = .;
        *(.data*)
        . = ALIGN(4);
        _b_data_end = .;
    } > DATA AT > CODE
    _b_data_load = LOADADDR(.data);
    .bss (NOLOAD) : {
        _b_bss = .;
        *(.bss*) *(COMMON)
        . = ALIGN(4);
        _b_bss_end = .;
    } > DATA
    /* heap (the decoder arena) from here up to the stack reserve; B_STACK is
       per image (Makefile: --defsym), the stack is painted and its low-water
       mark reported (b_main.c) */
    _b_heap = ALIGN(_b_bss_end, 8);
    _b_stack_top = ORIGIN(DATA) + LENGTH(DATA);
    _b_stack_limit = _b_stack_top - B_STACK;
    ASSERT(_b_heap + 0x8000 <= _b_stack_limit, "core-B image leaves less than 32 KiB of heap")
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.attributes) }
}
