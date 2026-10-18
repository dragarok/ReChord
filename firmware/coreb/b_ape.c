/* Core B: Monkey's Audio (APE 3.97 .. 3.99, fast .. insane) with Rockbox's libdemac
 * (integer only). Core A streams a 24-byte header (audio/ape_info.h) and then
 * only the frames; they form one bitstream, each frame starting where the last
 * one's range decoder stopped, so no seek table is needed. Output: the sample
 * left-justified in the 32-bit I2S word. Exact sample count: b_trk.frames.
 * Levels above fast: the NN filters go to the HIFI block when RECHORD.CFG hifi=
 * allows it (b_hifi_ape.c, docs/HIFI.md); each track ends with a line in core A's
 * log: core B's cycles per second of audio, and the block's share. */
#include "b_rt.h"
#include "b_hifi.h"
#include "b_hifi_fn.h"
#include "audio/ape_info.h"
#include "demac/demac.h"

#define CHUNK   256u                 /* blocks per decode_chunk() */
/* Worst case per block and channel: five range-coder steps of up to 4 bytes each.
 * Below this much unread input the buffer is topped up before the next chunk, so a
 * damaged frame cannot make the decoder read past it. */
#define IN_KEEP (CHUNK * 2u * 20u)
#define IN_BUF  0x4000u              /* frame bytes buffered */
#define IN_PAD  16u                  /* zeros after the last byte: the range decoder looks ahead */

typedef struct {
    struct ape_ctx_t ctx;            /* includes the predictor history (2.3 KiB) */
    int32_t d0[CHUNK], d1[CHUNK];
    uint32_t pos, len;               /* bytes consumed (a multiple of 4: word grouping) / held */
    uint32_t frame, left;            /* next frame to start; blocks left in the current one */
    uint32_t shift;                  /* 32 - bits per sample */
    int firstbyte;                   /* libdemac's byte within the current 32-bit word */
    int end;                         /* the buffer holds the rest of the track */
    uint8_t buf[IN_BUF + IN_PAD];
} ape_t;

static ape_t *s_ape;
static struct {
    uint32_t cyc, frames;                        /* decode cycles, frames (for the track's line) */
    int hifi;                                    /* the filters on the block this track */
} s_trk;

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

int b_ape_open(void)
{
    uint8_t h[APE_STREAM_HDR];
    ape_t *a;

    if (b_in_read(h, sizeof h) != sizeof h || memcmp(h, "MACb", 4))
        return -1;
    s_ape = a = b_calloc(sizeof *a);
    if (!a)
        return -1;
    a->ctx.fileversion = (int16_t)rd16(h + 4);
    a->ctx.compressiontype = (uint16_t)rd16(h + 6);
    a->ctx.blocksperframe = rd32(h + 8);
    a->ctx.finalframeblocks = rd32(h + 12);
    a->ctx.totalframes = rd32(h + 16);
    a->ctx.bps = (uint16_t)rd16(h + 20);
    a->ctx.channels = (uint16_t)rd16(h + 22);
    if ((uint32_t)a->ctx.fileversion < APE_VERSION_MIN || (uint32_t)a->ctx.fileversion > APE_VERSION_MAX ||
        a->ctx.channels != b_trk.channels || a->ctx.bps != b_trk.bits || a->ctx.bps > 24 ||
        a->ctx.compressiontype % 1000u || a->ctx.compressiontype < 1000u || a->ctx.compressiontype > 5000u ||
        !a->ctx.totalframes)
        return -2;
    a->shift = 32u - a->ctx.bps;
    a->firstbyte = 3;                /* little-endian 32-bit words, read from the top byte */
    b_in_need = IN_BUF;
    memset(&s_trk, 0, sizeof s_trk);
    s_trk.hifi = b_hifi_ape_open(&a->ctx);
    return 0;
}

/* Keep the unread bytes (from a word boundary), append what the ring has of the
 * track, waiting for core A only if less than IN_KEEP would be held. */
static void ape_fill(ape_t *a)
{
    uint32_t rest, n;

    if (a->end)
        return;
    if (a->pos) {
        a->len -= a->pos;
        memmove(a->buf, a->buf + a->pos, a->len);
        a->pos = 0;
    }
    rest = b_trk.bytes - b_trk.skip - b_in_pos();    /* of the track, not yet in the buffer */
    n = b_in_avail();
    if (a->len + n < IN_KEEP)
        n = IN_KEEP - a->len;                        /* b_in_read() waits for it */
    if (n > IN_BUF - a->len)
        n = IN_BUF - a->len;
    if (n > rest)
        n = rest;
    a->len += b_in_read(a->buf + a->len, n);
    if (n == rest) {
        a->end = 1;
        memset(a->buf + a->len, 0, IN_PAD);
    }
}

uint32_t b_ape_read(int32_t *dst, uint32_t frames)
{
    ape_t *a = s_ape;
    uint32_t got = 0, ch = a->ctx.channels, t0 = b_cycles();
    int used;

    while (got < frames) {
        uint32_t n = frames - got;

        if (a->len - a->pos < IN_KEEP)
            ape_fill(a);
        if (!a->left) {                              /* start the next frame */
            if (a->frame >= a->ctx.totalframes)
                break;
            a->left = a->frame + 1u == a->ctx.totalframes ? a->ctx.finalframeblocks : a->ctx.blocksperframe;
            a->ctx.currentframeblocks = (int)a->left;
            init_frame_decoder(&a->ctx, a->buf + a->pos, &a->firstbyte, &used);
            a->pos += (uint32_t)used;
            a->frame++;
            continue;
        }
        if (n > a->left)
            n = a->left;
        if (n > CHUNK)
            n = CHUNK;
        decode_chunk(&a->ctx, a->buf + a->pos, &a->firstbyte, &used, a->d0, a->d1, (int)n);
        a->pos += (uint32_t)used;
        if (a->pos > a->len + IN_PAD) {              /* ran past the track: damaged, end it */
            a->frame = a->ctx.totalframes;
            a->left = 0;
            break;
        }
        if (s_trk.hifi && b_hifi_ape_lost()) {       /* the block failed mid-frame: silence */
            memset(a->d0, 0, n * 4u);
            memset(a->d1, 0, n * 4u);
            b_hifi_fn_stats.lost += n;
        }
        if (ch == 2) {
            for (uint32_t i = 0; i < n; i++) {
                dst[2 * (got + i)] = (int32_t)((uint32_t)a->d0[i] << a->shift);
                dst[2 * (got + i) + 1] = (int32_t)((uint32_t)a->d1[i] << a->shift);
            }
        } else {
            for (uint32_t i = 0; i < n; i++)
                dst[got + i] = (int32_t)((uint32_t)a->d0[i] << a->shift);
        }
        a->left -= n;
        got += n;
    }
    s_trk.cyc += b_cycles() - t0;
    s_trk.frames += got;
    return got;
}

/* The track's line: core B's decode cycles per second of audio (the clock APE needs), and what the
 * block did. */
void b_ape_close(void)
{
    const b_hifi_fn_stats_t *h = &b_hifi_fn_stats;
    uint32_t mhz100, wait100;

    if (!s_trk.frames || !b_trk.rate || !s_ape)
        return;
    mhz100 = (uint32_t)((uint64_t)s_trk.cyc * b_trk.rate / s_trk.frames / 10000u);
    wait100 = (uint32_t)((uint64_t)h->wait_cyc * b_trk.rate / s_trk.frames / 10000u);
    b_msg("ape: track %u, level %u, %u-bit: core B %u.%02u M cycles/s, NN filters %s (%u jobs, wait %u.%02u M/s)",
          (unsigned)b_trk.seq, (unsigned)s_ape->ctx.compressiontype, (unsigned)b_trk.bits, (unsigned)(mhz100 / 100u),
          (unsigned)(mhz100 % 100u), s_trk.hifi ? "on HIFI" : "in software", (unsigned)h->jobs,
          (unsigned)(wait100 / 100u), (unsigned)(wait100 % 100u));
    b_hifi_ape_close();
}
