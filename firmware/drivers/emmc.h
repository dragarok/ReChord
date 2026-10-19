/*
 * emmc.h -- eMMC block access (DW-MMC at 0x60030000).
 * Sector = 512 bytes; buffers must be 4-byte aligned.
 */
#ifndef RECHORD_EMMC_H
#define RECHORD_EMMC_H

#include <stdint.h>

/* 0 on success, negative = failing init step (see emmc_rknanoc.c). */
int emmc_init(void);
/* What emmc_init() found: "eMMC", "eMMC, byte addressed", "SD NAND (SDHC|SDSC)"; data lines used. */
const char *emmc_kind(void);
int emmc_width(void);
uint32_t emmc_sectors(void);
int emmc_read(uint32_t lba, uint32_t count, void *buf);
/* Only LBAs >= the user volume start found by emmc_layout(); -30 otherwise. */
int emmc_write(uint32_t lba, uint32_t count, const void *buf);

/* The write-behind queue (emmc_rknanoc.c): the main loop never waits while the card programs.
 * emmc_write_async(): as emmc_write(), but the blocks are copied into a queue (in order; waits only
 * when it is full; more than half the queue at once is written at once, after it). emmc_wq_step():
 * once per main-loop pass: sees the block in flight done, and with may_start sends the next one;
 * returns the blocks still queued. emmc_busy(): a block is being programmed (a read now waits for
 * it). emmc_sync(): everything on the card (power off, USB sessions, reboot), 0 or -32 if a queued
 * write failed. emmc_quiesce(): the block in flight finished (for code that drives the controller
 * itself: bootcrumb.c). emmc_read() and emmc_write() see and keep the queue's order. */
int emmc_write_async(uint32_t lba, uint32_t count, const void *buf);
int emmc_wq_step(int may_start);
int emmc_busy(void);
unsigned emmc_wq_pending(void);
unsigned emmc_wq_free(void);
int emmc_sync(void);
void emmc_quiesce(void);
typedef struct {
    uint32_t queued, merged, written, errors, waits, syncs, stuck, peak;
    int last_error;
    uint32_t wait_lr, wait_ms;      /* the last read that waited for a write: its caller, how long */
} emmc_wq_stats_t;
const emmc_wq_stats_t *emmc_wq_stats(void);

/* Loader layout from the IDB: firmware area = [0x2000, user_start),
 * user volume = [user_start, emmc_sectors()). 0 on success. */
int emmc_layout(uint32_t *user_start);

/* Firmware area (IDB system size; 0 before emmc_layout() succeeded), two
 * slots of emmc_fw_sectors()/2. Writes are bounded to the area; -30 else. */
#define EMMC_FW_BASE 0x2000u
uint32_t emmc_fw_sectors(void);
int emmc_fw_write(uint32_t rel, uint32_t count, const void *buf);

/* microSD slot (second DW-MMC instance). sd_init(): 0 = ready, -1 = no card, other negative =
 * failing step. sd_read / sd_write: card sectors (SDSC cards are byte addressed inside), 0 or a
 * negative step; SD_ERR_NOCARD when the card-detect pin says the card is out (then nothing is sent).
 * sd_written(): sectors written since boot (what a USB disk session wrote, by difference). */
#define SD_ERR_NOCARD (-40)
int sd_present(void);
int sd_init(void);
uint32_t sd_sectors(void);           /* 0 = not initialized */
int sd_is_hc(void);
int sd_bus_width(void);
int sd_read(uint32_t lba, uint32_t count, void *buf);
int sd_write(uint32_t lba, uint32_t count, const void *buf);
uint32_t sd_written(void);

#endif
