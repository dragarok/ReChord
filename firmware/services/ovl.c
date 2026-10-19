/*
 * ovl.c -- the overlay windows on core A (ovl.h).
 *
 * Four windows (firmware_ap.ld): the modes' window (OVL_WIN: the USB DAC, USB-C earphones, the
 * Developer tests, the library's tag reader, one at a time), the library's (OVL_LIB: the music
 * library alone), the file system's (OVL_FS: the writer's cold half, fsw, alone) and the library
 * job's phase window (OVL_LPH: its walk or its build, carved from the library's own range). The
 * second and third are loaded at their first use and never replaced, so they never take a window
 * from USB-C earphones playing, and code in any window can call the file system's writer; the
 * fourth is swapped by the library's job alone (services/lib_job.c), between its phases.
 *
 * Loading one: the one in its window leaves its mode (its stop hook, while it is still there), is
 * checked unchanged (a window holds code and constants only: its objects' .data and .bss stay
 * resident, firmware_ap.ld), any hook that still points into it is cleared, then the new one is
 * read from the module table (services/modules.c: the module's word sum) and checked: magic, its
 * id, its length, the ABI of this link and the CRC of every byte. A copy that is damaged or
 * unreadable is read again from the second firmware slot (the updater writes the same image to
 * both; services/fw_update.c). Anything still wrong is a refusal: the window marked empty, a log
 * line once per boot, the caller told (NULL), never a jump into what was read.
 *
 * Load time (the device: core B's 99688 B image loads in 9..17 ms, ECHOLOG 2026-10-16/17 "B up
 * with bmain"; an overlay is under a third of that) holds the main loop like a core-B image load
 * does, with the input ring (64 KiB) and the PCM ring (8 blocks) still feeding the output; the
 * log line has the time the device measures ("ovl: <name> <bytes> B <ms> ms").
 */
#include <stddef.h>
#include <stdint.h>

#include "ovl.h"
#include "modules.h"
#include "services/fw_update.h"
#include "services/log.h"
#include "drivers/clock.h"

extern uint8_t __ovl_win_start[], __ovl_win_end[];   /* firmware_ap.ld OVL_WIN */
extern uint8_t __ovl_lib_start[], __ovl_lib_end[];   /* firmware_ap.ld OVL_LIB */
extern uint8_t __ovl_fs_start[], __ovl_fs_end[];     /* firmware_ap.ld OVL_FS: fsw's alone */
extern uint8_t __ovl_lph_start[], __ovl_lph_end[];   /* firmware_ap.ld OVL_LPH: the library job's phase */
extern const char __ovl_abi[];                      /* absolute: this link's ABI (Makefile) */

static uint8_t *const s_start[4] = { __ovl_win_start, __ovl_lib_start, __ovl_fs_start, __ovl_lph_start };
static uint8_t *const s_end[4] = { __ovl_win_end, __ovl_lib_end, __ovl_fs_end, __ovl_lph_end };
#define HDR(w) ((volatile ovl_hdr_t *)s_start[w])

static unsigned s_cur[4];              /* OVL_NONE or the overlay in each window */
static unsigned s_hold;                /* calls running in the modes' window now (ovl_hold) */
static uint32_t s_bad;                 /* refused for good this boot: bit per id */
static const char *s_why[OVL_COUNT];

uint32_t ovl_crc(const void *p, uint32_t n)
{
    return fwu_crc(0, (const uint8_t *)p, n);
}

unsigned ovl_current(void)
{
    return s_cur[0];
}

const char *ovl_why(unsigned id)
{
    return id < OVL_COUNT ? s_why[id] : NULL;
}

const void *ovl_if(unsigned id)
{
    unsigned w = id < OVL_COUNT ? ovl_desc[id].win : 0u;

    return id != OVL_NONE && id == s_cur[w] ? HDR(w)->api : NULL;
}

void ovl_hold(int d)
{
    s_hold += (unsigned)d;
}

/* The CRC an overlay carries: of all its len bytes, its own crc field counted as zero (so the
 * header's api pointer is covered too). */
static uint32_t whole_crc(unsigned w, uint32_t len)
{
    static const uint8_t zero[4];
    const uint8_t *b = s_start[w];
    uint32_t c = ovl_crc(b, offsetof(ovl_hdr_t, crc));

    c = fwu_crc(c, zero, 4);
    return fwu_crc(c, b + offsetof(ovl_hdr_t, crc) + 4u, len - offsetof(ovl_hdr_t, crc) - 4u);
}

static int in_window(unsigned w, uint32_t a)   /* (window w's own range: evicting the library's phase
                                               * code once cleared USB-C earphones' hook in window 0) */
{
    return a >= (uint32_t)s_start[w] && a < (uint32_t)s_end[w];
}

/* The overlay in window w leaves it: its mode stopped, the window checked unchanged, the hooks
 * that still point into it cleared. */
static void evict(unsigned w)
{
    unsigned id = s_cur[w];
    uint32_t len = HDR(w)->len;

    if (id == OVL_NONE)
        return;
    if (ovl_desc[id].stop)
        ovl_desc[id].stop();
    s_cur[w] = OVL_NONE;
    if (HDR(w)->magic != OVL_MAGIC || len < sizeof(ovl_hdr_t) || len > (uint32_t)(s_end[w] - s_start[w]) ||
        whole_crc(w, len) != HDR(w)->crc)
        log_printf("ovl: %s changed while loaded (something else wrote the window)", ovl_desc[id].module);
    for (unsigned i = 0; ovl_hooks[i]; i++)
        if (in_window(w, (uint32_t)*ovl_hooks[i])) {
            *ovl_hooks[i] = NULL;
            log_printf("ovl: %s left hook %u pointing into the window: cleared", ovl_desc[id].module, i);
        }
}

/* Read overlay id from firmware slot `slot` into its window and check it; its length, or < 0
 * with *why set. */
static int load(unsigned id, unsigned slot, const char **why)
{
    unsigned w = ovl_desc[id].win;
    uint32_t room = (uint32_t)(s_end[w] - s_start[w]) & ~511u;
    int n = module_load_slot(slot, ovl_desc[id].module, s_start[w], room);

    if (n < 0) {
        *why = n == -2 ? "not in the image" : n == -3 ? "too big" : n == -4 ? "damaged (sum)" : "read failed";
        return n;
    }
    if ((uint32_t)n < sizeof(ovl_hdr_t) || HDR(w)->magic != OVL_MAGIC || HDR(w)->id != id ||
        HDR(w)->len > (uint32_t)n || HDR(w)->len < sizeof(ovl_hdr_t)) {
        *why = "damaged (header)";
        return -5;
    }
    if (HDR(w)->abi != (uint32_t)__ovl_abi) {
        *why = "built for another image";
        return -6;
    }
    if (whole_crc(w, HDR(w)->len) != HDR(w)->crc) {
        *why = "damaged (CRC)";
        return -7;
    }
    return (int)HDR(w)->len;
}

const void *ovl_get(unsigned id)
{
    const char *why = NULL;
    uint32_t us;
    uint64_t t0;
    unsigned w;
    int n;

    if (id == OVL_NONE || id >= OVL_COUNT)
        return NULL;
    w = ovl_desc[id].win;
    if (id == s_cur[w])
        return HDR(w)->api;
    if (s_bad >> id & 1u)
        return NULL;
    if (s_hold && w == 0) {            /* code in the window is running: it stays */
        log_printf("ovl: %s wanted while %s runs: refused", ovl_desc[id].module,
                   s_cur[0] ? ovl_desc[s_cur[0]].module : "-");
        s_why[id] = "busy";
        return NULL;
    }
    evict(w);
    t0 = time_us();
    n = load(id, 0, &why);
    if (n < 0 && n != -3 && n != -6) {  /* damaged or unreadable: the second slot holds the same image */
        log_printf("ovl: %s: slot 1's copy %s (%d): reading slot 2's", ovl_desc[id].module, why, n);
        n = load(id, 1, &why);
    }
    us = (uint32_t)(time_us() - t0);
    if (n < 0) {
        HDR(w)->magic = 0;             /* nothing of it counts as loaded */
        s_bad |= 1u << id;
        s_why[id] = why;
        log_printf("ovl: %s refused: %s (%d)", ovl_desc[id].module, why, n);
        return NULL;
    }
    s_cur[w] = id;
    log_printf("ovl: %s %d B %u.%u ms", ovl_desc[id].module, n, (unsigned)(us / 1000u), (unsigned)(us % 1000u / 100u));
    return HDR(w)->api;
}

void ovl_run(unsigned id, unsigned n)
{
    void (*const *fn)(void) = ovl_get(id);

    if (!fn)
        return;
    s_hold++;
    fn[n]();
    s_hold--;
}
