/*
 * sdcard.h -- the microSD card as the rest of the firmware sees it: detected, debounced, mounted
 * read-write as volume 1 ("SD"), unmounted when it goes, and events for whoever keeps an index of
 * it (the music library). services/sdcard.c.
 */
#ifndef RECHORD_SDCARD_H
#define RECHORD_SDCARD_H

#include <stdint.h>

enum { SD_EV_NONE = 0, SD_EV_INSERTED = 1, SD_EV_REMOVED = 2, SD_EV_CHANGED = 3 };

/* The newest event not read yet, once (one slot: an unread one is replaced by a newer one, so an
 * INSERTED tells which card is in now whatever came before it; compare serials). INSERTED = a card
 * is in and mounted (*serial = its volume serial); REMOVED = the card went, volume 1 is unmounted
 * (*serial = the one it had); CHANGED = a USB disk session wrote to the card, it is mounted again;
 * NONE = nothing new. serial may be NULL. */
int sd_event(uint32_t *serial);
int sdcard_mounted(void);            /* 1 = volume 1 is a mounted card */
uint32_t sdcard_serial(void);        /* its volume serial, 0 if none */
const char *sdcard_text(void);       /* the root menu's row: "no card", "1024M HC 4b exFAT", ... */

/* Every main-loop pass (startup/ap_ui.c): the card-detect pin sampled every 100 ms; an insertion or
 * a removal counts once the pin reads the new state on three samples in a row (RECHO 0.9.2 main
 * loop 0x03051238..0x030512fe). Never waits. 1 when the card's state changed (redraw). */
int sdcard_tick(void);

/* Around a USB disk session (startup/ap_ui.c usb_session): before it, everything on the card and
 * volume 1 unmounted (the PC owns the card); after it, mounted again, CHANGED if the PC wrote. */
void sdcard_usb_begin(void);
void sdcard_usb_end(void);

#endif
