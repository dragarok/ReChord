/* Core B: WMA Lossless (mono, stereo; 16 and 24 bit) with FFmpeg's decoder made
 * standalone (coreb/wma/lossless, README.rechord).
 *
 * Input: the ASF stream of b_asf.h, as media objects; each object is one WMA
 * Lossless packet (block_align bytes), taken a frame at a time as FFmpeg's decode
 * loop does; at the end of the track the decoder is flushed for its last frame.
 * Output: the samples left-justified in the 32-bit I2S word (bit exact).
 *
 * Only b_rt.h is used, so the host reference (tests/wmaref.c) builds this file
 * as it is. */
#include "b_asf.h"
#include "wma/lossless/wmalldec.h"

typedef struct {
    b_asf_t asf;
    asf_waveformatex_t wfx;
    const uint8_t *data;                 /* rest of the current packet */
    uint32_t left;
    int shift;                           /* sample -> I2S word */
    int flushing, ended;
    uint32_t out_pos, out_n;             /* frames in the decoder's buffers to hand out */
    uint32_t errors;
} wmall_t;

static wmall_t *s;

uint32_t b_wmall_errors(void) { return s ? s->errors + s->asf.errors : 0; }

static void *alloc(size_t n) { return b_alloc(n); }

int b_wmall_open(void)
{
    const asf_cfg_t *c;
    int r;

    s = b_calloc(sizeof *s);
    if (!s)
        return -1;
    if ((r = b_asf_open(&s->asf)) != 0)
        return r;
    c = &s->asf.cfg;
    if (c->codec_id != ASF_WMALOSSLESS || c->channels > 2)
        return -2;
    s->wfx.packet_size = c->packet_size;
    s->wfx.audiostream = c->stream;
    s->wfx.codec_id = c->codec_id;
    s->wfx.channels = c->channels;
    s->wfx.rate = c->rate;
    s->wfx.bitrate = c->bitrate;
    s->wfx.blockalign = c->block_align;
    s->wfx.bitspersample = c->bits;
    s->wfx.datalen = c->extra_len;
    memcpy(s->wfx.data, c->extra, c->extra_len);
    if (wmall_init(&s->wfx, alloc) < 0)
        return -3;
    s->shift = wmall_bits() == 16 ? 16 : 0;  /* 24-bit samples come << 8 already */
    return 0;
}

uint32_t b_wmall_read(int32_t *dst, uint32_t frames)
{
    uint32_t got = 0, ch = s->asf.cfg.channels;

    while (got < frames) {
        if (s->out_pos < s->out_n) {
            uint32_t n = s->out_n - s->out_pos;
            const int32_t *l = wmall_samples(0) + s->out_pos, *r = wmall_samples(ch > 1) + s->out_pos;

            if (n > frames - got)
                n = frames - got;
            for (uint32_t i = 0; i < n; i++) {
                if (ch > 1) {
                    dst[2 * (got + i)] = (int32_t)((uint32_t)l[i] << s->shift);
                    dst[2 * (got + i) + 1] = (int32_t)((uint32_t)r[i] << s->shift);
                } else {
                    dst[got + i] = (int32_t)((uint32_t)l[i] << s->shift);
                }
            }
            s->out_pos += n;
            got += n;
            continue;
        }
        if (s->ended)
            break;
        if (s->left || s->flushing) {        /* the packet's next frame (or the flush) */
            int n = 0, r = wmall_decode(s->flushing ? NULL : s->data, s->flushing ? 0 : (int)s->left, &n);

            if (s->flushing) {
                if (n <= 0 || ++s->flushing > 4)
                    s->ended = 1;            /* no more frames */
            } else if (r < 0) {
                s->errors++;                 /* drop the rest of this packet */
                s->left = 0;
            } else if (r == 0 && n <= 0) {
                s->left = 0;                 /* no progress: next packet */
            } else {
                s->data += r;
                s->left = (uint32_t)r < s->left ? s->left - (uint32_t)r : 0;
            }
            s->out_pos = 0;
            s->out_n = n > 0 ? (uint32_t)n : 0;
            continue;
        }
        {
            const uint8_t *d;
            uint32_t len;

            if (!b_asf_object(&s->asf, &d, &len)) {
                s->flushing = 1;             /* end of the track: the last frame */
                continue;
            }
            s->data = d;
            s->left = len;
        }
    }
    return got;
}
