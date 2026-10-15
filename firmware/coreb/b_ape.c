/* Core B: Monkey's Audio (APE 3.97 .. 3.99, fast .. insane) with Rockbox's libdemac
 * (integer only). Core A streams a 24-byte header (audio/ape_info.h) and then
 * only the frames; they form one bitstream, each frame starting where the last
 * one's range decoder stopped, so no seek table is needed. Output: the sample
 * left-justified in the 32-bit I2S word. Exact sample count: b_trk.frames. */
#include "b_rt.h"
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
    uint32_t got = 0, ch = a->ctx.channels;
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
    return got;
}
