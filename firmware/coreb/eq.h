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

/* Stock's presets as the fewest sections (eq_design.c k_fit; at 32 kHz and up): each curve's RBJ peaking
 * sections with a free centre, Q and gain, fitted by snowsky_echo_mini tests/eqfit.c (MINIV390.IMG) to
 * within 0.5 dB of both the curve as stock's bands design it at every rate (stock's gain rows at
 * 0x030464C8, RBJ peaking, Q 1/sqrt 2) and stock's own 44.1 kHz coefficients (IMG +0xC2450), 20 Hz..20
 * kHz: tests/eqref.c checks the firmware's design of them. X(arg, mode, hz x 4, 1/2Q in Q30,
 * A = 10^(dB/40) in Q30); worst error (eqfit) in each row's comment. */
#define EQ_FIT_TABLE(X, a) \
    X(a, EQ_RETRO,  39904,  744726536,  761164406)   /* 9975.98 Hz, Q 0.7209, -5.9769 dB: 0.137 dB */ \
    X(a, EQ_RETRO,   1261, 2906811221, 1162821215)   /*  315.13 Hz, Q 0.1847, +1.3845 dB */ \
    X(a, EQ_BASS,     319,  973379871, 1583925288)   /*   79.70 Hz, Q 0.5516, +6.7534 dB: 0.251 dB */ \
    X(a, EQ_HEAVY,  12847,  551091810,  883739419)   /* 3211.74 Hz, Q 0.9742, -3.3830 dB: 0.340 dB */ \
    X(a, EQ_HEAVY,   2107, 1620562733, 1328258675)   /*  526.76 Hz, Q 0.3313, +3.6953 dB */ \
    X(a, EQ_POP,    40012,  760546257, 1276078152)   /* 10002.89 Hz, Q 0.7059, +2.9991 dB: 0.004 dB */ \
    X(a, EQ_POP,    12600,  760637576,  903405617)   /* 3150.10 Hz, Q 0.7058, -3.0007 dB */ \
    X(a, EQ_POP,      400,  758417332, 1351580278)   /*  100.00 Hz, Q 0.7079, +3.9977 dB */ \
    X(a, EQ_JAZZ,    4487,  969928896, 1334888772)   /* 1121.80 Hz, Q 0.5535, +3.7818 dB: 0.284 dB */ \
    X(a, EQ_JAZZ,    1259,  664721045,  745202590)   /*  314.63 Hz, Q 0.8077, -6.3450 dB */ \
    X(a, EQ_JAZZ,     390,  820055105, 1348826617)   /*   97.61 Hz, Q 0.6547, +3.9623 dB */ \
    X(a, EQ_JAZZ,   39491,  887317863, 1502526551)   /* 9872.63 Hz, Q 0.6050, +5.8369 dB */ \
    X(a, EQ_UNIQUE, 12774,  725890995, 1277075151)   /* 3193.58 Hz, Q 0.7396, +3.0127 dB: 0.061 dB */ \
    X(a, EQ_UNIQUE,   399,  722005090, 1277219148)   /*   99.76 Hz, Q 0.7436, +3.0146 dB */ \
    X(a, EQ_UNIQUE,  3989,  730549897,  904856834)   /*  997.37 Hz, Q 0.7349, -2.9728 dB */
#define EQ_FIT_RATE  32000u                      /* below it: stock's bands themselves (eq_design.c) */
#define EQ_FIT_CNT_(m, mode, hz4, kq, A)  + ((mode) == (m))
#define EQ_FIT_N(m)  (0 EQ_FIT_TABLE(EQ_FIT_CNT_, m))

/* The sections each mode runs at 44.1 kHz (the fits above), for core A's CPU budget (services/cpuclk.c);
 * Custom: all ten (its equal neighbours may merge: eq_design.c). Core B counts the design itself. */
#define EQ_PRESET_SECTIONS { 0, EQ_FIT_N(EQ_RETRO), EQ_FIT_N(EQ_BASS), EQ_FIT_N(EQ_HEAVY), EQ_FIT_N(EQ_POP), \
                             EQ_FIT_N(EQ_JAZZ), EQ_FIT_N(EQ_UNIQUE), EQ_BANDS }

/* Core-B QEMU instructions per output frame (stereo): EQ_INSN_FRAME + EQ_INSN_SECTION per section in
 * use, rounded up from the measured 9.9 + 26.5 a section (QEMU b-cpi=2.6, the EQ's own cycles a frame
 * on CD16.flac, b_eq.c's line: Bass's 1 section 92.8 cycles, Retro's 2 163.6, Jazz's 4 302.3, Custom's
 * 10 715.3; snowsky_echo_mini docs/MERGE_NOTES_eqfit.md; before the one-pass kernel: 34 + 32). A
 * crossfade runs two sets for ~21 ms. */
#define EQ_INSN_FRAME    12u
#define EQ_INSN_SECTION  27u

/* One section: y = x + d (x - x2) + a1 (x1 - y1) + a2 (x2 - y2), the RBJ cookbook peaking filter
 * (b0 = 1 + d, b1 = a1, b2 = 1 + a2 - b0): three multiplies, unity at DC and Nyquist exactly whatever
 * the rounding. Q3.29. eq.c runs it as w = y - x (w = d (x - x2) - a1 w1 - a2 w2), the same words. */
typedef struct { int32_t d, a1, a2; } eq_coef_t;
typedef struct { int32_t x1, x2, w1, w2; uint32_t e; } eq_state_t;   /* w = y - x; e: the error fed back (Q29 fraction) */

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
/* The sections eq_design() would make (no design: for core B's CPU budget, b_eq.c) */
int eq_count(unsigned mode, const int8_t *gain, uint32_t rate);

/* A new curve (mode, gain) at the current rate: a crossfade into it (from bypass, out to bypass, or
 * between two curves). 1 if anything changes, 0 if not, -1 while a crossfade runs (ask again after). */
int eq_set(eq_t *e, unsigned mode, const int8_t *gain);
/* A new stream at this rate (a track opened after the output stopped, or another rate) with this
 * curve: the bank designed for it, every state zero, no crossfade (a crossfade in flight is dropped). */
void eq_restart(eq_t *e, uint32_t rate, unsigned mode, const int8_t *gain);
/* The frames (stereo I2S words, sample left-justified, L then R) in place. Not called in bypass. */
void eq_process(eq_t *e, int32_t *words, uint32_t frames);

#endif
