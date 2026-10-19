/*
 * lib.h -- the music library (docs/LIBRARY.md): its state as resident code sees it, and the calls
 * into the library overlay (startup/ap_ovl.c, OVL_LIB in window 2). Resident code calls only the
 * ap_lib_* gates; everything else here runs in the overlay.
 */
#ifndef RECHORD_LIB_H
#define RECHORD_LIB_H

#include <stdint.h>

#include "fs.h"

enum { LIB_DATA_SD = 0, LIB_DATA_INTERNAL = 1 };       /* config libdata= sd | internal */
enum { LIB_EV_GONE, LIB_EV_MOUNTED, LIB_EV_WROTE, LIB_EV_SUSPEND, LIB_EV_RESUME };

typedef struct {
    uint8_t data;                    /* LIB_DATA_*: where the card's index and the lists go */
    uint8_t busy, busy_vol, pct;     /* indexing now: which volume, how far */
    uint8_t mounted[2], writable[2]; /* volumes 0 Internal, 1 SD */
    uint8_t lists_vol, notice;       /* where the lists are now; a fallback notice is due */
    uint32_t changed;                /* counts index switches and volume changes (the views reload) */
} lib_status_t;
extern lib_status_t g_lib;           /* resident (startup/ap_ovl.c) */

/* ---- resident gates (startup/ap_ovl.c): each loads the library overlay once ---- */
void ap_lib_boot(void);              /* after the mounts and the settings */
void ap_lib_poll(void);              /* every main-loop pass: the job's slice */
void ap_lib_volume(int vol, int ev); /* a card in or out (sd_probe), a USB disk session */
void ap_lib_open(void);              /* Home > Library */
int ap_lib_home_value(char *out, int n);   /* the Home row's value */
void ap_lib_np(int what);            /* Now Playing's Options: LIB_NP_* */
void ap_lib_data_changed(void);      /* Settings > Library data: lists to the new place */
int ap_lib_np_fav(void);             /* 1 when the playing track is a favourite */
enum { LIB_NP_FAV, LIB_NP_ADDPL };

/* ---- in the overlay ---- */
void lib_boot(void);
int lib_step(void);
void lib_vol_event(int vol, int ev);
void lib_request(int vol, int full);
int lib_busy(int *vol, int *pct);
int lib_lists_vol(void);
const char *lib_data_dir(int v);
int lib_view_other(int vol);
int idx_load_at(int vol, const char *dir, const char *base);
void lib_forget_handles_vol(int vol);
void lib_data_changed(void);
void lib_ui_open(void);
int lib_ui_home_value(char *out, int n);
void lib_ui_np(int what);
int lib_ui_np_fav(void);

#endif
