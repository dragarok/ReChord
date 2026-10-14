/* The core-B image (build/coreb/coreb.bin), copied to 0x01000000 by audio.c. */
    .section .rodata.coreb, "a"
    .balign 4
    .global coreb_image, coreb_image_end
coreb_image:
    .incbin "build/coreb/coreb.bin"
coreb_image_end:
