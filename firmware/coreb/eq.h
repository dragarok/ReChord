/*
 * eq.h -- the EQ: up to 10 peaking sections after the decoder, on core B (coreb/eq.c; the glue is
 * coreb/b_eq.c, core A's side audio/audio.c, services/cpuclk.c, ui/screens.c). Plain C with no
 * core-B dependency: the test model (snowsky_echo_mini tests/eqref.c) compiles this very file, so
 * what core B plays is checked bit for bit against it.
 *
 * Modes: Off, stock's six presets (names and curves: stock MINIV390, docs/MERGE_NOTES_eq.md in the
 * project repo), Custom (10 octave bands, 31.25 Hz .. 16 kHz, -12..+12 dB).
 */
#ifndef RECHORD_EQ_H
#define RECHORD_EQ_H

#include <stdint.h>

#define EQ_BANDS     10            /* sections at most (Custom's octave bands) */
#define EQ_GAIN_MAX  12            /* dB, Custom's range (stock's own band tables: -12..+12, 25 levels) */
#define EQ_CHUNK     128           /* frames processed at a time (eq_process): two buffers of 1 KiB */

/* stock's mode numbers and order (MINIV390: RKEQAdjust 0x03089B80, its TBB table at 0x03089C2C; the
 * names are string ids 0xF8..0xFF, "Disabled" .. "Custom") */
enum { EQ_OFF = 0, EQ_RETRO, EQ_BASS, EQ_HEAVY, EQ_POP, EQ_JAZZ, EQ_UNIQUE, EQ_CUSTOM, EQ_MODES };

/* The sections each preset runs (eq.c k_preset), for core A's CPU budget; Custom: its non-zero bands. */
#define EQ_PRESET_SECTIONS { 0, 4, 2, 3, 3, 5, 3, 10 }

/* Core-B QEMU instructions per output frame (stereo): EQ_INSN_FRAME + EQ_INSN_SECTION per section in
 * use. Measured (QEMU, the coreprof plugin, steady playback, CD16.flac: EQ off 102, Jazz's 5 sections
 * 296, Custom's 10 456 a frame; snowsky_echo_mini docs/MERGE_NOTES_eq.md). A crossfade runs two sets
 * for ~21 ms. */
#define EQ_INSN_FRAME    34u
#define EQ_INSN_SECTION  32u

/* One section: y = x + d (x - x2) + a1 (x1 - y1) + a2 (x2 - y2), the RBJ cookbook peaking filter
 * (b0 = 1 + d, b1 = a1, b2 = 1 + a2 - b0): three multiplies, unity at DC and Nyquist exactly whatever
 * the rounding. Q3.29. */
typedef struct { int32_t d, a1, a2; } eq_coef_t;
typedef struct { int32_t x1, x2, y1, y2; uint32_t e; } eq_state_t;   /* e: the error fed back (Q29 fraction) */

typedef struct {
    uint32_t rate;
    int n;                            /* sections (0: the identity) */
    int32_t pre;                      /* preamp, Q30 (1 << 30 = 0 dB) */
    int pre_milli;                    /* the same x 1000, for the log */
    int prime;                        /* its state from the first frame it runs on (a crossfade's new curve) */
    int zero;                         /* every state is 0 (flushed in silence): silence in, silence out */
    eq_coef_t c[EQ_BANDS];
    eq_state_t s[2][EQ_BANDS];        /* [channel][section] */
} eq_bank_t;

/* The engine: the bank playing and, during a crossfade, the one coming in. */
typedef struct {
    eq_bank_t bank[2];
    uint8_t cur;                      /* the bank playing */
    uint8_t on;                       /* 0: bypass (nothing runs, the words pass untouched) */
    uint8_t ramp;                     /* a crossfade from bank[cur] to bank[cur ^ 1] */
    uint8_t ramp_log2;                /* its length, 2^n frames (~21 ms) */
    uint32_t pos;                     /* frames into it */
    uint32_t rate;                    /* the rate the banks are designed for (0: none yet) */
    uint32_t spos;                    /* frames run since the stream started: the chunks are aligned to
                                       * it (EQ_CHUNK), so how the frames arrive never changes a word */
    uint32_t loud;                    /* a non-zero word in the aligned chunk so far */
    uint32_t mode;                    /* the curve in effect (or coming in) */
    int8_t gain[EQ_BANDS];
    int32_t buf[2][EQ_CHUNK * 2];
} eq_t;

/* Design a bank: 0 sections for Off or a flat curve. Bands at or above 0.45 x rate are left out. */
int eq_design(eq_bank_t *b, unsigned mode, const int8_t *gain, uint32_t rate);

/* A new curve (mode, gain) at the current rate: a crossfade into it (from bypass, out to bypass, or
 * between two curves). 1 if anything changes, 0 if not, -1 while a crossfade runs (ask again after). */
int eq_set(eq_t *e, unsigned mode, const int8_t *gain);
/* A new stream at this rate (a track opened after the output stopped, or another rate) with this
 * curve: the bank designed for it, every state zero, no crossfade (a crossfade in flight is dropped). */
void eq_restart(eq_t *e, uint32_t rate, unsigned mode, const int8_t *gain);
/* The frames (stereo I2S words, sample left-justified, L then R) in place. Not called in bypass. */
void eq_process(eq_t *e, int32_t *words, uint32_t frames);

#endif
