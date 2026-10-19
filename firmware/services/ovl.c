/*
 * ovl.c -- the overlay window on core A (ovl.h).
 *
 * One overlay in the window at a time. Loading one: the one in the window leaves its mode (its
 * stop hook, while it is still there), is checked unchanged (the window holds code and constants
 * only: its objects' .data and .bss stay resident, firmware_ap.ld), any hook that still points
 * into it is cleared, then the new one is read from the module table (services/modules.c: the
 * module's word sum) and checked: magic, its id, its length, the ABI of this link and the CRC of
 * every byte. A copy that is damaged or unreadable is read again from the second firmware slot
 * (the updater writes the same image to both; services/fw_update.c). Anything still wrong is a
 * refusal: the window marked empty, a log line once per boot, the caller told (NULL), never a
 * jump into what was read.
 *
 * Load time (the device: core B's 99688 B image loads in 9..17 ms, ECHOLOG 2026-10-16/17 "B up
 * with bmain"; an overlay is under a third of that) holds the main loop like a core-B image load
 * does, with the input ring (64 KiB) and the PCM ring (8 blocks) still feeding the output; the
 * log line has the time the device measures ("ovl: <name> <bytes> B <ms> ms").
 *
 * The second window (OVL_FS, 0x03048000) holds the file-system writer's overlay (fsw) and nothing
 * else: loaded the first time a write needs it and never evicted, so it coexists with whatever
 * runs in the first window (USB-C earphones, a mode's overlay calling the file system). Its
 * checks and its refusal are the same.
 */
#include <stddef.h>
#include <stdint.h>

#include "ovl.h"
#include "modules.h"
#include "services/fw_update.h"
#include "services/log.h"
#include "drivers/clock.h"

extern uint8_t __ovl_win_start[], __ovl_win_end[];   /* firmware_ap.ld OVL_WIN */
extern uint8_t __ovl_fs_start[], __ovl_fs_end[];     /* firmware_ap.ld OVL_FS: fsw's alone */
extern const char __ovl_abi[];                      /* absolute: this link's ABI (Makefile) */

#define HDR ((volatile ovl_hdr_t *)__ovl_win_start)
#define HDR_OF(id) ((volatile ovl_hdr_t *)win_of(id))

static unsigned s_cur;                 /* OVL_NONE or the overlay in the window */
static int s_fsw;                      /* fsw is in the second window */

static uint8_t *win_of(unsigned id)
{
    return id == OVL_FSW ? __ovl_fs_start : __ovl_win_start;
}
static unsigned s_hold;                /* calls running in the window now (ovl_hold) */
static uint32_t s_bad;                 /* refused for good this boot: bit per id */
static const char *s_why[OVL_COUNT];

uint32_t ovl_crc(const void *p, uint32_t n)
{
    return fwu_crc(0, (const uint8_t *)p, n);
}

unsigned ovl_current(void)
{
    return s_cur;
}

const char *ovl_why(unsigned id)
{
    return id < OVL_COUNT ? s_why[id] : NULL;
}

const void *ovl_if(unsigned id)
{
    if (id == OVL_FSW)
        return s_fsw ? HDR_OF(id)->api : NULL;
    return id != OVL_NONE && id == s_cur ? HDR->api : NULL;
}

void ovl_hold(int d)
{
    s_hold += (unsigned)d;
}

/* The CRC an overlay carries: of all its len bytes, its own crc field counted as zero (so the
 * header's api pointer is covered too). */
static uint32_t whole_crc(const uint8_t *w, uint32_t len)
{
    static const uint8_t zero[4];
    uint32_t c = ovl_crc(w, offsetof(ovl_hdr_t, crc));

    c = fwu_crc(c, zero, 4);
    return fwu_crc(c, w + offsetof(ovl_hdr_t, crc) + 4u, len - offsetof(ovl_hdr_t, crc) - 4u);
}

static int in_window(uint32_t a)
{
    return a >= (uint32_t)__ovl_win_start && a < (uint32_t)__ovl_win_end;
}

/* The overlay in the window leaves it: its mode stopped, the window checked unchanged, the hooks
 * that still point into it cleared. */
static void evict(void)
{
    unsigned id = s_cur;
    uint32_t len = HDR->len;

    if (id == OVL_NONE)
        return;
    if (ovl_desc[id].stop)
        ovl_desc[id].stop();
    s_cur = OVL_NONE;
    if (HDR->magic != OVL_MAGIC || len < sizeof(ovl_hdr_t) || len > (uint32_t)(__ovl_win_end - __ovl_win_start) ||
        whole_crc(__ovl_win_start, len) != HDR->crc)
        log_printf("ovl: %s changed while loaded (something else wrote the window)", ovl_desc[id].module);
    for (unsigned i = 0; ovl_hooks[i]; i++)
        if (in_window((uint32_t)*ovl_hooks[i])) {
            *ovl_hooks[i] = NULL;
            log_printf("ovl: %s left hook %u pointing into the window: cleared", ovl_desc[id].module, i);
        }
}

/* Read overlay id from firmware slot `slot` and check it; its length, or < 0 with *why set. */
static int load(unsigned id, unsigned slot, const char **why)
{
    uint8_t *w = win_of(id);
    volatile ovl_hdr_t *h = HDR_OF(id);
    uint32_t room = (uint32_t)((id == OVL_FSW ? __ovl_fs_end : __ovl_win_end) - w) & ~511u;
    int n = module_load_slot(slot, ovl_desc[id].module, w, room);

    if (n < 0) {
        *why = n == -2 ? "not in the image" : n == -3 ? "too big" : n == -4 ? "damaged (sum)" : "read failed";
        return n;
    }
    if ((uint32_t)n < sizeof(ovl_hdr_t) || h->magic != OVL_MAGIC || h->id != id || h->len > (uint32_t)n ||
        h->len < sizeof(ovl_hdr_t)) {
        *why = "damaged (header)";
        return -5;
    }
    if (h->abi != (uint32_t)__ovl_abi) {
        *why = "built for another image";
        return -6;
    }
    if (whole_crc(w, h->len) != h->crc) {
        *why = "damaged (CRC)";
        return -7;
    }
    return (int)h->len;
}

const void *ovl_get(unsigned id)
{
    const char *why = NULL;
    uint32_t us;
    uint64_t t0;
    int n;

    if (id == OVL_NONE || id >= OVL_COUNT)
        return NULL;
    if (id == OVL_FSW ? s_fsw : id == s_cur)
        return HDR_OF(id)->api;
    if (s_bad >> id & 1u)
        return NULL;
    if (id != OVL_FSW) {               /* (the second window has no one to make room for) */
        if (s_hold) {                  /* code in the window is running: it stays */
            log_printf("ovl: %s wanted while %s runs: refused", ovl_desc[id].module,
                       s_cur ? ovl_desc[s_cur].module : "-");
            s_why[id] = "busy";
            return NULL;
        }
        evict();
    }
    t0 = time_us();
    n = load(id, 0, &why);
    if (n < 0 && n != -3 && n != -6) {  /* damaged or unreadable: the second slot holds the same image */
        log_printf("ovl: %s: slot 1's copy %s (%d): reading slot 2's", ovl_desc[id].module, why, n);
        n = load(id, 1, &why);
    }
    us = (uint32_t)(time_us() - t0);
    if (n < 0) {
        HDR_OF(id)->magic = 0;         /* nothing of it counts as loaded */
        s_bad |= 1u << id;
        s_why[id] = why;
        log_printf("ovl: %s refused: %s (%d)", ovl_desc[id].module, why, n);
        return NULL;
    }
    if (id == OVL_FSW)
        s_fsw = 1;
    else
        s_cur = id;
    log_printf("ovl: %s %d B %u.%u ms", ovl_desc[id].module, n, (unsigned)(us / 1000u), (unsigned)(us % 1000u / 100u));
    return HDR_OF(id)->api;
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
