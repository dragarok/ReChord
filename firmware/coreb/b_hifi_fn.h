/*
 * b_hifi_fn.h -- the HIFI block's APE and ALAC functions (b_hifi_fn.c: what both share; b_hifi_ape.c,
 * b_hifi_alac.c: each function; docs/HIFI.md sections 3b, 3c has every value with its source).
 *
 * Each function is used as stock's decoder uses it, only after its self-test reproduced the
 * decoder's own software, with a bounded wait, a check of its jobs against that software and the
 * software behind it; RECHORD.CFG hifi=auto|off|verify covers both (AS->hifi_mode).
 */
#ifndef RECHORD_B_HIFI_FN_H
#define RECHORD_B_HIFI_FN_H

#include <stdint.h>

/* HIFI registers and DMA2's transfer status (SDK Hw_hifi.h, Hw_dma.h; the values in docs/HIFI.md) */
#define HIFI_REG(o)       (*(volatile uint32_t *)(0x01060000u + (o)))
#define HIFI_R_DMACR      0x00u
#define HIFI_R_INTCR      0x04u
#define HIFI_R_INTSR      0x08u
#define HIFI_R_FUN_CFG    0x0Cu
#define HIFI_R_ACC_CLR    0x10u
#define HIFI_R_APE_CFG    0x20u
#define HIFI_R_ALAC_CFG   0x28u
#define HIFI_R_ALAC_COEF  0x9000u               /* "Alac initial coef address max=64x32bit" */
#define HIFI_FUN_START    3u                    /* TX_Transfer_Start | RX_Transfer_Start */
#define HIFI_INT_DONE     0x8000u
#define HIFI_DMACR_STOCK  0x01080108u           /* SDK Hifi_Set_ACC_Dmacr */
#define HIFI_INTCR_STOCK  0x00830083u           /* SDK Hifi_Set_ACC_Intcr */
#ifndef SYST_RVR
#define SYST_RVR          (*(volatile uint32_t *)0xE000E014u)   /* SysTick reload: 10 ms */
#endif

/* Counters for a track's lines, cleared at its open. */
typedef struct {
    uint32_t jobs, samples, wait_cyc, wait_max_cyc, polls;
    uint32_t checked;                           /* jobs checked against the software */
    uint32_t mismatch;                          /* of those, jobs that differed */
    uint32_t redone;                            /* jobs whose software result plays instead */
    uint32_t lost;                              /* APE: blocks silenced (a failure mid-frame) */
    uint32_t sw;                                /* jobs the decoder ran in software though the block was on */
} b_hifi_fn_stats_t;
extern b_hifi_fn_stats_t b_hifi_fn_stats;

/* At a track's open: 1 = the block may be tried (hifi= not off, the block not dead, core B not
 * reloaded mid-access: that marks it dead); powers it once per core B. */
int b_hifi_fn_ready(void);
/* hifi=off: the block's clocks off once per core B (stock's state between tracks). */
void b_hifi_fn_off(void);
/* Before a job: hifi_busy set (a stalled bus shows at the next core B's open). */
void b_hifi_fn_busy(void);
/* Wait for a job's RX transfer (and INTSR bit 15 when `done`), from t0 at most `lim` cycles. 0, or
 * < 0: timed out: the block is off until power-off (both functions and FLAC), logged with `what`. */
int b_hifi_fn_wait(uint32_t t0, uint32_t lim, int done, const char *what, uint32_t n);
/* The track's "hifi:" line (the mode, the function's state, the counters). */
void b_hifi_fn_track_line(const char *fn, const char *state);

/* ---- APE (b_hifi_ape.c, the "ape" image) ---- */
struct ape_ctx_t;
/* At the track's open (after the header): 1 = its NN filters go to the block this track. */
int b_hifi_ape_open(struct ape_ctx_t *ctx);
void b_hifi_ape_close(void);
/* libdemac's hooks (demac/decoder.c, DEMAC_RECHORD_HIFI): a frame starts (its filters were reset);
 * one channel's filters for `count` blocks: 1 = done by the block (or silenced), 0 = the software's. */
void demac_rechord_frame(struct ape_ctx_t *ctx);
int demac_rechord_filters(struct ape_ctx_t *ctx, int ch, int32_t *data, int count);
/* After decode_chunk(): 1 = this chunk is lost (a failure mid-frame took the filter state with it):
 * the caller plays silence for it. */
int b_hifi_ape_lost(void);

/* ---- ALAC (b_hifi_alac.c, the "main" image) ---- */
int b_hifi_alac_open(uint32_t frame_len);
void b_hifi_alac_close(void);

#endif
