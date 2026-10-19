/*
 * ovl.h -- code overlays on core A: a mode's code loaded on demand from the firmware slot.
 *
 * Core A's resident code is capped by what the boot ROM loads: FW_CODE in firmware_ap.ld ends
 * where the reference image's resident span ends, below the ROM's own copies into the fonts
 * window (tools/pack_fw1.py rows 0 and 2), so it cannot grow. Stock has the same limit and the
 * same answer: its modules m00..m78 load into code windows on demand, each CRC-checked
 * (snowsky_echo_mini docs/RESEARCH.md "Overlay modules", docs/IMAGE_FORMAT.md entries 1-63).
 *
 * The window is core-A SRAM no one else uses (firmware_ap.ld OVL_WIN, docs/MEMORY_MAP.md), one
 * overlay in it at a time. Each overlay is an output section of the one core-A link (so it
 * calls resident code directly, and the linker checks every call), stored in the image's module
 * table (tools/make_fw1.py --module) by tools/ovl_pack.py, which also sets its CRC. Resident
 * code never names a symbol inside an overlay (NOCROSSREFS_TO in firmware_ap.ld fails the link
 * if it does): it reaches one only through ovl_get(), which returns the overlay's own table of
 * entry points (startup/ap_ovl.c has the tables and the gates in front of them).
 *
 * What may go in an overlay: code that runs in a mode the user enters and leaves (the USB DAC,
 * USB-C earphones, the Developer tests), never what the recovery path needs (boot, the boot
 * guard, Recovery, the updater and slot repair, the way back to stock, the rockusb route, the
 * crash record): those must work with every overlay in the slot damaged. docs/MEMORY_MAP.md:
 * how to put a feature in one.
 */
#ifndef RECHORD_OVL_H
#define RECHORD_OVL_H

#include <stdint.h>

/* OVL_FSW: the file-system writer (services/fs_write_ext.c), the second window's only one (ovl.c) */
enum { OVL_NONE = 0, OVL_USBDAC, OVL_USBOUT, OVL_DEV, OVL_FSW, OVL_COUNT };

#define OVL_MAGIC 0x314C564Fu        /* "OVL1" */

/* The first bytes of every overlay (startup/ap_ovl.c, placed first by firmware_ap.ld). */
typedef struct {
    uint32_t magic;                  /* OVL_MAGIC */
    uint32_t abi;                    /* __ovl_abi of the link it came from (the Makefile: a hash
                                      * of every core-A object and the linker script) */
    uint32_t id;                     /* OVL_* */
    uint32_t len;                    /* bytes, this header included */
    uint32_t crc;                    /* ovl_crc() of all len bytes, this field as 0 (tools/ovl_pack.py) */
    const void *api;                 /* the overlay's entry points (its struct, startup/ap_ovl.c) */
} ovl_hdr_t;

/* One per overlay, resident (startup/ap_ovl.c). */
typedef struct {
    const char *module;              /* its name in the module table (Makefile OVL_MODULES) */
    const char *what;                /* for the refusal: "USB DAC" */
    void (*stop)(void);              /* leaves its mode before another overlay takes the window
                                      * (runs while it is still loaded); NULL: nothing to leave */
} ovl_desc_t;

extern const ovl_desc_t ovl_desc[OVL_COUNT];
/* Words in resident RAM an overlay may point into the window (the USB interrupt's hook): cleared,
 * and the fault logged, before the window is overwritten. NULL-terminated (startup/ap_ovl.c). */
extern void *volatile *const ovl_hooks[];
/* The overlay's entry points, loaded first if it is not in the window. NULL = refused: missing,
 * damaged, built for another resident image (then never read again this boot, logged once), or
 * the window busy (code in it is still running). Never from an interrupt. */
const void *ovl_get(unsigned id);
/* The entry points if it is loaded now, else NULL; never loads (for calls that only matter while
 * its mode runs: a poll, a status). */
const void *ovl_if(unsigned id);
/* ovl_get() then entry n of an overlay whose api is a table of void (*)(void). */
void ovl_run(unsigned id, unsigned n);
unsigned ovl_current(void);          /* OVL_NONE or the overlay in the window */
const char *ovl_why(unsigned id);    /* why ovl_get(id) last refused ("damaged (CRC)"), or NULL */
/* +1 before a call into the window, -1 after it: while any runs, no other overlay is loaded
 * (the gates in startup/ap_ovl.c do it; ovl_run() too). */
void ovl_hold(int d);

/* The CRC the headers carry: services/fw_update.c's (poly 0x04C10DB7, MSB first, init 0). */
uint32_t ovl_crc(const void *p, uint32_t n);

#endif
