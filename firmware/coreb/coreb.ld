/*
 * Core-B image. Core A copies it to 0x01000000 (the code RAM; core B sees it at
 * 0), so code and read-only data are linked at 0 with the vector table first.
 * Everything written lives in the data RAM (audio_shared.h): .data (loaded from
 * the copy behind .text by b_reset) and .bss from 0x01020000, the stack above
 * them up to the shared block.
 */
ENTRY(b_reset)

MEMORY
{
    CODE (rx)  : ORIGIN = 0x00000000, LENGTH = 0x00020000   /* AS_B_IMAGE_MAX */
    DATA (rw)  : ORIGIN = 0x01020000, LENGTH = 0x00008000   /* AS_B_DATA .. AS_SHARED */
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
    _b_stack_top = ORIGIN(DATA) + LENGTH(DATA);
    ASSERT(_b_bss_end + 0x3000 <= _b_stack_top, "core-B data leaves less than 12 KiB of stack (libmad needs ~6)")
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.attributes) }
}
