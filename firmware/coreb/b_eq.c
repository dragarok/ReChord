/*
 * b_eq.c -- the EQ on core B (eq.h): core A's curve (audio_shared.h eq_*) on what the decoder wrote,
 * before the words go into the PCM ring (b_main.c produce). One curve for the stream: gapless joins
 * at the same rate keep every filter's state; a stream that starts again (a stop, a seek, another
 * rate: the output drained first) starts from zero.
 *
 * Per track, the EQ runs only if its sections fit core B's CPU budget at the point the track plays
 * at (room(): core A's estimate of the format, as_track_t eq; none on DSD, as stock); a curve with
 * more plays bypassed on that track, and a change at a gapless join is crossfaded like any other. Bypass costs a compare a decode step (b_main.c), nothing a sample.
 */
#include "b_rt.h"
#include "eq.h"

eq_t b_eq;
uint32_t b_eq_seq;                      /* the AS->eq_seq the engine has */
uint32_t b_eq_dirty;                    /* a track opened: its room (its point, its format) taken again */
uint64_t b_eq_cyc;                      /* the track's EQ cycles (b_main.c) */
static uint32_t s_frames;               /* the track's frames through the EQ */

static const char *const k_name[EQ_MODES] = { "Off", "Retro", "Bass", "Heavy", "Pop", "Jazz", "Unique", "Custom" };

static unsigned s_mode, s_want, s_room;  /* the curve A set, its sections, the track's room for them */

/* The sections that fit on this track: 60 % of core B's clock at the point it plays at (96 MHz at
 * the UI point, 448 at the FLAC point: drivers/clock.h; clk_cur 1 = CLOCK_POINT_FLAC) less the format
 * (b_trk.eq: core A's estimate, services/cpuclk.c), in the device's 2.6 cycles a QEMU instruction
 * (cpuclk.h CPUCLK_DEVICE_CPI_X100). None on DSD: stock runs no EQ there (its gate at MINIV390
 * 0x03066942 passes codecs below 8 only, DSD being 8 and 9). */
static unsigned room(void)
{
    uint64_t have = (AS->clk_cur == 1u ? 448000000ull : 96000000ull) * 60u,      /* (both x100) */
             need = (uint64_t)(b_trk.eq + EQ_INSN_FRAME) * b_trk.rate * 260u, n;

    if (b_trk.codec == AS_CODEC_DSD || !b_trk.rate || need >= have)
        return 0;
    n = (have - need) / ((uint64_t)EQ_INSN_SECTION * b_trk.rate * 260u);
    return n > EQ_BANDS ? EQ_BANDS : (unsigned)n;
}

/* the curve A set, or Off when this track has no room for it at the point it plays at */
static unsigned target(int8_t *g)
{
    static const uint8_t k_n[EQ_MODES] = EQ_PRESET_SECTIONS;
    const volatile int8_t *src = (const volatile int8_t *)AS->eq_src;   /* g_cfg.eq, then its gains */
    unsigned m = src && (uint8_t)src[0] < EQ_MODES ? (uint8_t)src[0] : EQ_OFF, n = k_n[m];

    s_mode = m;
    s_room = room();
    for (int k = 0; k < EQ_BANDS; k++)
        g[k] = src ? src[1 + k] : 0;
    if (m == EQ_CUSTOM)
        for (int k = n = 0; k < EQ_BANDS; k++)
            n += g[k] != 0;
    s_want = n;
    return n > s_room ? EQ_OFF : m;
}

static void said(const char *what)
{
    const eq_bank_t *b = &b_eq.bank[b_eq.cur ^ (b_eq.ramp ? 1u : 0u)];

    if (s_want > s_room)
        b_msg("eq: %s off at %u Hz: %s needs %u sections, this track has room for %u (core B at %u MHz)", what,
              (unsigned)b_eq.rate, k_name[s_mode], s_want, s_room, (unsigned)(AS->b_hz / 1000000u));
    else
        b_msg("eq: %s %s at %u Hz: %u sections, preamp x0.%03u", what, k_name[b_eq.mode % EQ_MODES],
              (unsigned)b_eq.rate, (unsigned)b->n, (unsigned)(b->pre_milli >= 1000 ? 999 : b->pre_milli));
}

/* the track has opened (b_main.c track_open): a stream starting at its rate starts from zero; at the
 * same rate (a gapless join) the next decode step takes the curve again, for this track's room */
void b_eq_open(void)
{
    s_frames = 0;
    b_eq_cyc = 0;
    b_eq_dirty = 1;
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
    b_eq.rate = 0;
    b_eq.on = 0;
    b_eq.ramp = 0;
}
