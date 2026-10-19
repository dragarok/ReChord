/*
 * ReChord AP (fw1) linker script -- clean-room A_CORE image (milestone 1).
 *
 * VERIFIED BOOT MODEL (byte audit 2026-10-11, docs/HANDOVER.md "PHASE A
 * AUDIT RESULT"; supersedes the earlier XIP assumption):
 *
 *   The loader (Mask ROM/sec2) loads the fw1 IMG body CONTIGUOUSLY into RAM:
 *       RAM = 0x0304F298 + IMG offset
 *   (IMG 0x1F8 -> RAM 0x0304F490; for any image whose vector table sits at
 *   IMG 0xD68 the base is exactly 0x03050000 - 0xD68).  Scatter-row "src"
 *   fields are addresses in that loaded RAM image, NOT flash XIP addresses.
 *
 *   Entry contract: the vector table must land at IMG 0xD68 == RAM 0x03050000
 *   (the RKnanoFW header base).  The payload therefore starts with a 0x5B0
 *   byte prefix before the vector table (docs/fw1-packing.md).  The ROM reads
 *   word0 = initial MSP and word1 = reset from it.
 *
 * Memory roles (matched to the reference image's roles):
 *
 *   0x03000000..0x03004000  AP MAIN STACK only (SP = 0x03004000, grows down).
 *                           NEVER place data here (2026-08-25 brick cause B).
 *   0x03004000..            SYS_DATA: .data (VMA) then .bss (NOLOAD).
 *   0x03050000..            FW_CODE: .vectors + .text + .rodata + .data LMA
 *                           image (the whole blob is loaded in place and
 *                           executes in place - the reference does exactly
 *                           this; its startup even holds a pointer to itself).
 *
 * .data is loaded from an LMA inside the code image (AT> FW_CODE) and copied
 * to 0x03004000.. by PowerOn_Reset (bounded copy, the reference recipe).
 * The LMA image is contiguous with the end of .text so the payload stays one
 * flat blob.  _data_load_start = LOADADDR(.data).
 *
 * .bss is NOLOAD: the ARMCC zero_init attribute is ignored by GCC, so the
 * *_BSS_ named sections land in .bss (NOBITS) and must NOT be part of the
 * payload (docs/fw1-packing.md section 6).  PowerOn_Reset zeroes the bounded
 * .bss span itself (the reference does the same).
 *
 * The vector table is isolated in its own section (R-a2d2b19d3577; the rule's text is in no doc
 * or .dfiu revision: what docs/HANDOVER.md section 4 records is the SDK-era BB link, where
 * --gc-sections "silently drops nearly everything (stubs look dead)"). This link has every root
 * explicit -- ENTRY, KEEP for both vector tables, .noinit and the overlay headers -- and uses
 * --gc-sections (firmware/ap.mk): it drops only sections nothing references (2776 B of dead code
 * at its introduction, the list in docs/MEMORY_MAP.md).
 *
 * Overlays (services/ovl.h, docs/MEMORY_MAP.md): code for modes the user enters and leaves, run
 * from OVL_WIN, one at a time, loaded on demand from the image's module table. Each is an output
 * section of this link, so it calls resident code directly; the resident image never refers into
 * one (NOCROSSREFS_TO below fails the link), only through the overlay's header at OVL_WIN.
 */

ENTRY(PowerOn_Entry)

MEMORY
{
    /* FW_CODE: code + rodata + vector table + the .data load image.
       Executes in place from the loaded image at 0x03050000-based addresses.
       Length mirrors the reference's resident span (its row 0 ends at
       0x030771D4); the hard payload limit is 0x56AB8 (0x57068 minus the
       0x5B0 prefix) which is far above. */
    FW_CODE (rx)  : ORIGIN = 0x03050000, LENGTH = 0x00027820

    /* FW_RAM: initialized data + bss.  Starts at the top of the main stack
       (SP = 0x03004000); the 16 KiB BELOW 0x03004000 is the stack zone
       0x03000000..0x03004000 and must never hold data.  The upper bound
       (0x03030000) leaves 0x03030000..0x0303FFFF to the audio input ring
       (audio_shared.h AS_IN_RING, written by core A, read by core B) and stays
       clear of the loaded image (0x0304F490+). */
    FW_RAM  (rwx) : ORIGIN = 0x03004000, LENGTH = 0x0002C000   /* to 0x03030000 */

    /* OVL_WIN: the overlay window. Core-A SRAM between the audio input ring (to 0x0303FFFF) and
       the loaded image (0x0304F490): nothing of ours is there (the loader's data sits there
       while it boots us, SDK BuildAll.sct LOADER_DATA_BASE 0x0304A000, docs/community.md 4.2,
       and is dead once our code runs). 32 KiB; 0x03048000..0x0304F000 stays free for a second
       window. Holds code and read-only data only: an overlay's .data and .bss are resident. */
    OVL_WIN (rx)  : ORIGIN = 0x03040000, LENGTH = 0x00008000

    /* OVL_FS: the second window, beside the first in the same unused SRAM: the file-system
       writer's overlay (fsw) alone, loaded at its first use and never evicted, so it runs while
       any overlay is in OVL_WIN (services/ovl.c). 16 KiB; 0x0304C000..0x0304F000 stays free. */
    OVL_FS (rx)   : ORIGIN = 0x03048000, LENGTH = 0x00004000

    /* OVL_STORE: the overlays' load addresses. Only in the full ELF: no such memory, nothing
       loads from it; the image carries each overlay as a module (Makefile: objcopy -R strips
       them from what tools/make_fw1.py packs, tools/ovl_pack.py extracts them). */
    OVL_STORE (r) : ORIGIN = 0x90000000, LENGTH = 0x00100000
}

/* ---- stacks ----------------------------------------------------------- */
/* Initial MSP = the vector table's word 0 (byte-verified 0x03004000 in the
 * reference image); the stack is the 16 KiB below it and nothing else may
 * live there.  0x03000000 is the floor of AP RAM. */
__stack_top   = 0x03004000;
__stack_limit = __stack_top - 0x4000;   /* 0x03000000: the stack zone */

/* Keil scatter-file symbols the SDK-style startup references */
Image$$AP_MAIN_STACK$$ZI$$Limit = __stack_top;

SECTIONS
{
    /* ---- overlays (services/ovl.h, startup/ap_ovl.c) -------------------
       First in this script, so these input sections are theirs before .text's wildcards take
       the rest. Each starts with its header (OVL_HEADER). A file partly in an overlay is split by
       function section (-ffunction-sections): its statics shared with resident code stay in
       .text (an overlay may call them), and a function inlined into its caller goes with it.
       NOCROSSREFS: no overlay refers into another. */
    OVERLAY ORIGIN(OVL_WIN) : NOCROSSREFS AT (ORIGIN(OVL_STORE))
    {
        /* the USB DAC (UAC2): the driver and the session (startup/ap_ui.c) */
        .ovl_usbdac
        {
            KEEP(*(.ovl_hdr.usbdac))
            *(.ovl.usbdac.*)
            *usb_uac_rknanoc.o(.text* .rodata*)
            *ap_ui.o(.text.usb_dac_session* .rodata.usb_dac_session.* .text.uac_tick* .rodata.uac_tick.*
                     .text.uac_status* .text.dac_power_only* .rodata.dac_power_only.*)
            . = ALIGN(4);
        }
        /* USB-C earphones: the player as USB host */
        .ovl_usbout
        {
            KEEP(*(.ovl_hdr.usbout))
            *(.ovl.usbout.*)
            *usb_out_rknanoc.o(.text* .rodata*)
            . = ALIGN(4);
        }
        /* the Developer tests (Settings > Developer) */
        .ovl_dev
        {
            KEEP(*(.ovl_hdr.dev))
            *(.ovl.dev.*)
            *usb_cdc_rknanoc.o(.text* .rodata*)
            *hifi_probe.o(.text* .rodata*)
            *sd_selftest.o(.text* .rodata*)
            *bt_jieli.o(.text.bt_listen .rodata.bt_listen.*)
            *panel_io_rknanoc.o(.text.panel_te_probe .text.te_watch .rodata.panel_te_probe.*)
            /* (.rodata.usb_console_run.str1.1 and .rodata.usb_probe_menu.str1.1 stay resident:
               strings the compiler shares with resident tables in ap_ui.c) */
            *ap_ui.o(.text.dac_test_run* .rodata.dac_test_run.* .text.tone_burst* .rodata.sine64*
                     .text.usb_console_run* .text.cdc_* .rodata.cdc_*
                     .text.usb_probe_menu* .text.usb_probe_screen*
                     .rodata.usb_probe_screen.* .text.usb_detect_test* .rodata.usb_detect_test.*
                     .text.detect_wait* .rodata.detect_wait.* .text.bt_listen_test*
                     .rodata.bt_listen_test.* .text.listen_save* .rodata.listen_save.*
                     .text.te_probe_test*)
            . = ALIGN(4);
        }
    }

    /* the file-system writer's cold half (services/fs_write_ext.c), in the second window; its
       load address after the first window's overlays */
    .ovl_fsw ORIGIN(OVL_FS) : AT (ORIGIN(OVL_STORE) + 0x80000)
    {
        KEEP(*(.ovl_hdr.fsw))
        *(.ovl.fsw.*)
        *fs_write_ext.o(.text* .rodata*)
        . = ALIGN(4);
    }

    /* ---- vector table FIRST at the entry base -------------------------
       ap_vectors (ap_startup.c) must be the first bytes of the image:
       0x03050000 is where the loader lands IMG 0xD68 and where the ROM reads
       the vector table (VTOR requires 256-alignment).  The *ap_startup.o()
       spec pins it to the section head regardless of link order; vectors.c's
       legacy Route-B table ("vectors") is kept after it and is not wired. */
    .vectors ORIGIN(FW_CODE) : ALIGN(256)
    {
        KEEP(*ap_startup.o(.vectors))
        KEEP(*(.vectors))
    } > FW_CODE

    .text ALIGN(4) :
    {
        *(.text*)
        *(.rodata*)
        /* unwind metadata emitted by the ARM EABI compiler */
        *(.ARM.extab*)
        *(.ARM.exidx*)
        . = ALIGN(4);
    } > FW_CODE

    /* ---- SYS_DATA: initialized data ---------------------------------- */
    /* VMA at the top of the stack zone (0x03004000, the reference's row-1
     * window start); LMA continues the FW_CODE blob right after .text so the
     * loaded image stays contiguous and PowerOn_Reset can copy it down. */
    .data ORIGIN(FW_RAM) : ALIGN(4)
    {
        . = ALIGN(4);
        _data_start = .;
        KEEP(*(.data.ram_vectors))
        *(.data*)
        . = ALIGN(4);
        _data_end = .;
    } > FW_RAM AT> FW_CODE
    ASSERT(ap_ram_vectors == 0x03004000, "ROM enters via the VT at the start of .data")

    /* ---- SYS_DATA: zero-initialized data (bounded, NOLOAD) ----------- */
    /* PowerOn_Reset zeroes exactly [_bss_start, _bss_end); keep this span
       tight and 4-byte aligned at both ends (the clear loop is word-wise). */
    .bss ALIGN(4) (NOLOAD) :
    {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > FW_RAM

    /* ---- not cleared at boot: what a warm reset must keep (the crash record,
       the display trial's marker). In our own RAM, which the boot ROM leaves
       alone at power-on (device, rung 1: noise there), never in the ROM's own
       record at 0x0308FFE8 (docs/ROADMAP.md: the recovery path comes first). */
    /* At the top of FW_RAM, far from .bss: the loader's row 1 zeroes past
       .data, and whether it counts its last field from the start of .data
       (tools/pack_fw1.py) or from its end (the QEMU model) is not measured;
       either way the zeroing ends well below here. */
    .noinit (ORIGIN(FW_RAM) + LENGTH(FW_RAM) - 0x40) (NOLOAD) :
    {
        _noinit_start = .;
        KEEP(*(.noinit*))
        . = ALIGN(4);
        _noinit_end = .;
    } > FW_RAM
    ASSERT(_noinit_end <= ORIGIN(FW_RAM) + LENGTH(FW_RAM), ".noinit must fit in its 64 bytes")
    ASSERT(_bss_end + SIZEOF(.data) + 0x100 <= _noinit_start,
           ".noinit must stay clear of the loader's zeroing past .bss")

    /* Remaining SRAM between .bss and .noinit is the heap
       (symbolic only: no allocator is wired in milestone 1).  The stack zone
       below 0x03004000 is NOT heap. */
    __heap_start = _bss_end;
    __heap_end   = _noinit_start;

    /* Flash/load-resident address of the .data init image (inside the blob). */
    _data_load_start = LOADADDR(.data);

    /* Keil ZI symbols for SDK-style startup code: MUST be the real .bss
       span (pointing these at the whole region wipes .data at boot). */
    Image$$AP_SYS_DATA$$ZI$$Base   = _bss_start;
    Image$$AP_SYS_DATA$$ZI$$Length = _bss_end - _bss_start;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }
}

/* ---- the fonts window (drivers/text_load.c) -----------------------------
 * The "fonts" module (drivers/fonts/rechord_fonts.bin) is loaded at boot into
 * core-A SRAM from the end of FW_CODE (the most the code image may ever occupy:
 * the link fails before the code reaches it) to the audio LLI just below the PCM
 * ring (audio/audio_shared.h AS_LLI = 0x0308FF00). The ROM's boot-time copies
 * (the row-0 duplicate up by 0xB68 and the 4-byte poke at 0x03078CD0, tools/
 * pack_fw1.py) land here before our code runs; nothing uses the window after. */
__fw_code_end     = ORIGIN(FW_CODE) + LENGTH(FW_CODE);   /* (tools/ap_size.py) */
__fonts_ram_start = ALIGN(ORIGIN(FW_CODE) + LENGTH(FW_CODE), 256);
__fonts_ram_end   = 0x0308FF00;
ASSERT(__fonts_ram_end - __fonts_ram_start >= 0x17000,
       "the fonts window must hold the fonts module (about 92 KiB)")

/* ---- the overlays (services/ovl.h) ------------------------------------
 * The resident image never refers into an overlay: only through the header at OVL_WIN, which
 * services/ovl.c checks before anything in it runs. */
NOCROSSREFS_TO(.ovl_usbdac .vectors .text .data .ovl_fsw)
NOCROSSREFS_TO(.ovl_usbout .vectors .text .data .ovl_fsw)
NOCROSSREFS_TO(.ovl_dev .vectors .text .data .ovl_fsw)
NOCROSSREFS_TO(.ovl_fsw .vectors .text .data .ovl_usbdac .ovl_usbout .ovl_dev)
__ovl_fs_start = ORIGIN(OVL_FS);
__ovl_fs_end   = ORIGIN(OVL_FS) + LENGTH(OVL_FS);
ASSERT(__ovl_fs_start >= ORIGIN(OVL_WIN) + LENGTH(OVL_WIN), "the second window must not overlap the first")
ASSERT(__ovl_fs_end <= 0x0304F000, "the second window must stay below the loaded image (0x0304F490)")
ASSERT(ovl_hdr_fsw == ORIGIN(OVL_FS), "fsw must start with its header")
ASSERT(SIZEOF(.ovl_fsw) <= LENGTH(OVL_FS), "the fsw overlay is over its window")
__ovl_win_start = ORIGIN(OVL_WIN);
__ovl_win_end   = ORIGIN(OVL_WIN) + LENGTH(OVL_WIN);
ASSERT(ORIGIN(OVL_WIN) >= 0x03040000, "the overlay window must stay above the audio input ring")
ASSERT(__ovl_win_end <= 0x0304F000, "the overlay window must stay below the loaded image (0x0304F490)")
ASSERT(ovl_hdr_usbdac == ORIGIN(OVL_WIN) && ovl_hdr_usbout == ORIGIN(OVL_WIN) && ovl_hdr_dev == ORIGIN(OVL_WIN),
       "each overlay must start with its header")
ASSERT(SIZEOF(.ovl_usbdac) <= LENGTH(OVL_WIN), "the usbdac overlay is over the window")
ASSERT(SIZEOF(.ovl_usbout) <= LENGTH(OVL_WIN), "the usbout overlay is over the window")
ASSERT(SIZEOF(.ovl_dev) <= LENGTH(OVL_WIN), "the dev overlay is over the window")

/* ---- layout contracts (fail the link loudly on violation) ------------- */
ASSERT(ap_vectors == 0x03050000,
       "ap_vectors must sit at the fw1 entry base 0x03050000 (IMG 0xD68)")
ASSERT((ap_vectors & 0xFF) == 0, "ap_vectors must be 256-byte aligned (VTOR)")
ASSERT(PowerOn_Reset != 0, "PowerOn_Reset must be linked")
ASSERT(_data_start == 0x03004000,
       ".data must start at the stack top 0x03004000 (reference row-1 window)")
ASSERT(_data_start >= __stack_top,
       "no data may sit in the stack zone 0x03000000..0x03004000")
ASSERT(_bss_end <= ORIGIN(FW_RAM) + LENGTH(FW_RAM),
       "SYS_DATA overflow: .data+.bss must stay below 0x0304C000")
ASSERT(LOADADDR(.data) == ADDR(.text) + SIZEOF(.text),
       ".data LMA must be contiguous with the end of .text (flat blob)")
ASSERT(LOADADDR(.data) + SIZEOF(.data) <= ORIGIN(FW_CODE) + LENGTH(FW_CODE),
       "the .data load image must fit inside FW_CODE")
ASSERT(SIZEOF(.vectors) + SIZEOF(.text) + SIZEOF(.data) <= 0x56AB8,
       "payload blob exceeds the fw1 window (0x57068 minus the 0x5B0 prefix)")
