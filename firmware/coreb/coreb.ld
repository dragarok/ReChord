/*
 * Core B image. Core A copies it to 0x01000000; core B sees that RAM at 0, so
 * the image is linked at 0 with the vector table first. Code, data and bss
 * all live in the copy (no load/exec split); the stack is the top of the
 * image window. The decoder arena, the input ring and the shared block sit
 * above the window (firmware/audio/audio_shared.h).
 */
ENTRY(b_reset)

MEMORY
{
    BRAM (rwx) : ORIGIN = 0x00000000, LENGTH = 0x0001E000   /* AS_B_IMAGE_MAX */
}

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > BRAM
    .data : { *(.data*) . = ALIGN(4); } > BRAM
    .bss (NOLOAD) : {
        _b_bss = .;
        *(.bss*) *(COMMON)
        . = ALIGN(4);
        _b_bss_end = .;
    } > BRAM
    _b_stack_top = ORIGIN(BRAM) + LENGTH(BRAM);
    ASSERT(_b_bss_end + 0x3000 <= _b_stack_top, "core-B image leaves less than 12 KiB of stack (libmad needs ~6)")
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.attributes) }
}
