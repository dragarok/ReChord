/*
 * b_eq.c -- the EQ on core B (eq.h): core A's curve (audio_shared.h eq_*) on what the decoder wrote,
 * before the words go into the PCM ring (b_main.c produce). One curve for the stream: gapless joins
 * at the same rate keep every filter's state; a stream that starts again (a stop, a seek, another
 * rate: the output drained first) starts from zero.
 *
 * Per track, the EQ runs only if its sections fit core B's CPU budget at the point the track plays at
 * (room()): at the open from core A's estimate of the format (as_track_t eq) or, at a gapless join of
 * the same codec and rate, the last track's measured decoding; then every second from this track's own
 * measured decoding cycles (b_main.c guarded_read) and, while the EQ runs, its own measured cycles
 * (check()). A curve that comes to fit is crossfaded in; one that runs over the line (two seconds in a
 * row) is crossfaded out for the rest of the track (never back: no flapping). None on DSD, as stock. Bypass costs a compare a
 * decode step (b_main.c), nothing a sample.
 */
#include "b_rt.h"
#include "eq.h"

eq_t b_eq;
uint32_t b_eq_seq;                      /* the AS->eq_seq the engine has */
uint32_t b_eq_dirty;                    /* the curve to be taken again: a track opened, a check decided */
uint32_t b_eq_watch;                    /* a curve is set: the checks run (b_main.c calls b_eq_step) */
uint64_t b_eq_cyc;                      /* the track's EQ cycles (b_main.c) */
static uint32_t s_frames;               /* the track's frames through the EQ */

static const char *const k_name[EQ_MODES] = { "Off", "Retro", "Bass", "Heavy", "Pop", "Jazz", "Unique", "Custom" };

static unsigned s_mode, s_want, s_room;  /* the curve A set, its sections, the track's room for them */
static uint32_t s_dec100;               /* the decoder's measured cycles a frame x100 (0: not yet: A's estimate) */
static uint32_t s_codec, s_rate;        /* the codec and rate s_dec100 was measured on */
static uint8_t s_over;                  /* this track ran over the line with the EQ: off to its end */
static uint8_t s_overs;                 /* checks in a row over the line */
static uint32_t s_chk, s_fr0;           /* the next check's frame of the track; the last one's */
static uint64_t s_dec0, s_eq0;          /* the decoding and EQ cycles at the last check */

/* The CPU line: 60 % of core B's clock at the point the track plays at (96 MHz at the UI point, 448 at
 * the FLAC point: drivers/clock.h; clk_cur 1 = CLOCK_POINT_FLAC), x100 cycles a second */
#define LINE_PCT 60u
static uint64_t have100(void) { return (AS->clk_cur == 1u ? 448000000ull : 96000000ull) * LINE_PCT; }

/* The sections that fit on this track: the line less the format (its measured cycles a frame, or core
 * A's estimate b_trk.eq: services/cpuclk.c), in the device's 2.6 cycles a QEMU instruction for what is
 * not measured (cpuclk.h CPUCLK_DEVICE_CPI_X100): the EQ's EQ_INSN_FRAME + EQ_INSN_SECTION a section.
 * None on DSD: stock runs no EQ there (its gate at MINIV390 0x03066942 passes codecs below 8 only, DSD
 * being 8 and 9). */
static unsigned room(void)
{
    uint64_t have = have100(), dec = s_dec100 ? s_dec100 : (uint64_t)b_trk.eq * 260u,
             need = (dec + EQ_INSN_FRAME * 260u) * b_trk.rate, n;

    if (b_trk.codec == AS_CODEC_DSD || !b_trk.rate || need >= have)
        return 0;
    n = (have - need) / ((uint64_t)EQ_INSN_SECTION * b_trk.rate * 260u);
    return n > EQ_BANDS ? EQ_BANDS : (unsigned)n;
}

/* the curve A set, or Off when this track has no room for it at the point it plays at */
static unsigned target(int8_t *g)
{
    const volatile int8_t *src = (const volatile int8_t *)AS->eq_src;   /* g_cfg.eq, then its gains */
    unsigned m = src && (uint8_t)src[0] < EQ_MODES ? (uint8_t)src[0] : EQ_OFF;

    s_mode = m;
    s_room = room();
    for (int k = 0; k < EQ_BANDS; k++)
        g[k] = src ? src[1 + k] : 0;
    s_want = (unsigned)eq_count(m, g, b_trk.rate);   /* (the design's own count: fits, merges, the rate) */
    b_eq_watch = s_want && !s_over;
    return s_want > s_room || s_over ? EQ_OFF : m;
}

/* Every second of a track (b_eq_step): the last second's measured cycles a frame (a second's cycles fit
 * 32 bits up to 4.29 GHz). Running: decoding and the EQ over the line -> off for the rest of the track.
 * Waiting for room: this track's decoding is what room() counts from now on, and if the curve fits now
 * it is taken again. Logged at the first check and at a decision. Over the line means two seconds in a
 * row: a track's first second costs more (its decoder's start: Vorbis 1189 against 935 cycles a frame,
 * QEMU), which is no risk with the ring's 186 ms. */
static uint32_t per100(uint32_t cyc, uint32_t fr) { return cyc / fr * 100u + cyc % fr * 100u / fr; }

static void check(void)
{
    uint64_t dec = b_dec_cycles();
    uint32_t fr = AS->cur_frames - s_fr0, d100, e100, over;
    unsigned first = !s_fr0;

    d100 = per100((uint32_t)(dec - s_dec0), fr);
    e100 = per100((uint32_t)(b_eq_cyc - s_eq0), fr);
    s_dec0 = dec;
    s_eq0 = b_eq_cyc;
    s_fr0 = AS->cur_frames;
    s_chk = s_fr0 + b_trk.rate;
    s_dec100 = d100;
    s_codec = b_trk.codec;
    s_rate = b_trk.rate;
    over = b_eq.on && !b_eq.ramp && (uint64_t)(d100 + e100) * b_trk.rate > have100();
    s_overs = over ? s_overs + 1u : 0u;
    over = s_overs >= 2u;                          /* (two seconds running: not a track's first-second
                                                    * transient, which the ring's 186 ms rides out) */
    if (over)
        s_over = 1;
    if (over || (!b_eq.on && !s_over && s_want > s_room && s_want <= room()))
        b_eq_dirty = 1;                            /* (target() takes the curve again: off, or on as it fits now) */
    if (first || b_eq_dirty)
        b_msg("eq: track %u: decoding %u.%02u, EQ %u.%02u cycles a frame (measured): room for %u sections%s",
              (unsigned)b_trk.seq, (unsigned)(d100 / 100u), (unsigned)(d100 % 100u), (unsigned)(e100 / 100u),
              (unsigned)(e100 % 100u), room(), over ? ", over the line: off to its end" : "");
}

static void said(const char *what)
{
    const eq_bank_t *b = &b_eq.bank[b_eq.cur ^ (b_eq.ramp ? 1u : 0u)];

    if (s_over)
        b_msg("eq: %s off at %u Hz: %s over the line on this track", what, (unsigned)b_eq.rate, k_name[s_mode]);
    else if (s_want > s_room)
        b_msg("eq: %s off at %u Hz: %s needs %u sections, this track has room for %u (%s)", what,
              (unsigned)b_eq.rate, k_name[s_mode], s_want, s_room, s_dec100 ? "measured" : "estimate");
    else
        b_msg("eq: %s %s at %u Hz: %u sections, preamp x0.%03u, room %u (%s)", what, k_name[b_eq.mode % EQ_MODES],
              (unsigned)b_eq.rate, (unsigned)b->n, (unsigned)(b->pre_milli >= 1000 ? 999 : b->pre_milli), s_room,
              s_dec100 ? "measured" : "estimate");
}

/* the track has opened (b_main.c track_open): a stream starting at its rate starts from zero; at the
 * same rate (a gapless join) the next decode step takes the curve again, for this track's room */
void b_eq_open(void)
{
    s_frames = 0;
    b_eq_cyc = 0;
    b_eq_dirty = 1;
    s_over = s_overs = 0;
    s_fr0 = 0;
    s_dec0 = s_eq0 = 0;
    s_chk = b_trk.rate;                            /* (the first check after a second of the track) */
    if (b_trk.rate != s_rate || b_trk.codec != s_codec)
        s_dec100 = 0;                              /* (another codec or rate than measured: A's estimate first) */
    if (b_trk.rate != b_eq.rate) {
        int8_t g[EQ_BANDS];
        uint32_t seq = AS->eq_seq;
        unsigned m;

        __asm__ volatile ("dmb" ::: "memory");     /* (the curve A wrote before that seq) */
        m = target(g);
        eq_restart(&b_eq, b_trk.rate, m, g);
        b_eq_seq = seq;
        b_eq_dirty = 0;
        AS->eq_ack = seq;
        if (s_mode)
            said("start");
    }
}

/* a decode step's frames (stereo words), before they count as written */
void b_eq_step(int32_t *dst, uint32_t frames)
{
    if (b_eq_watch && AS->cur_frames >= s_chk)
        check();
    if (AS->eq_seq != b_eq_seq || b_eq_dirty) {
        int8_t g[EQ_BANDS];
        uint32_t seq = AS->eq_seq;
        int r;

        __asm__ volatile ("dmb" ::: "memory");
        r = eq_set(&b_eq, target(g), g);
        if (r >= 0) {                              /* (-1: a crossfade runs; the next step asks again) */
            b_eq_seq = seq;
            b_eq_dirty = 0;
            AS->eq_ack = seq;
            if (r)
                said("to");
            if (r)                                 /* (where: the test model starts its crossfade there) */
                b_msg("eq: crossfade from frame %u of track %u", (unsigned)AS->cur_frames, (unsigned)b_trk.seq);
        }
    }
    if (b_eq.on) {
        eq_process(&b_eq, dst, frames);
        s_frames += frames;
    }
}

/* the track is over (b_main.c track_close): its EQ line */
void b_eq_close(uint32_t hz)
{
    if (s_frames && hz) {
        uint64_t cpf100 = b_eq_cyc * 100u / s_frames;        /* cycles a frame, x100 */
        uint64_t mps = cpf100 * b_trk.rate / 100000u;         /* k cycles/s */

        b_msg("eq: track %u: %s, %u frames, %u.%02u cycles a frame, %u.%02u M cycles/s at %u Hz",
              (unsigned)b_trk.seq, k_name[b_eq.mode % EQ_MODES], (unsigned)s_frames,
              (unsigned)(cpf100 / 100u), (unsigned)(cpf100 % 100u), (unsigned)(mps / 1000u),
              (unsigned)(mps % 1000u / 10u), (unsigned)b_trk.rate);
    }
    s_frames = 0;
    b_eq_cyc = 0;
}

/* the stream stopped (a stop, a seek): the next track starts it again from zero */
void b_eq_stop(void)
{
    b_eq_watch = 0;                                /* (s_dec100 kept: a seek, the same codec and rate) */
    b_eq.rate = 0;
    b_eq.on = 0;
    b_eq.ramp = 0;
}
