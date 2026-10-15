/* Core B: WMA v1/v2 with Rockbox's fixed-point libwma (coreb/wma/README.rechord).
 *
 * Input: the ASF stream of b_asf.h, as media objects. An object is one or more
 * superframes of block_align bytes, a superframe one or more frames of frame_len
 * samples (with the bit reservoir, a frame may start in the previous superframe).
 * Output: the decoder's 2.29 fixed point (Rockbox's sample depth 29: 1.0 = 2^29),
 * polarity restored (it comes out inverted), clipped to +-1.0 and left-justified
 * in the 32-bit I2S word.
 *
 * Only b_rt.h is used, so the host reference (tests/wmaref.c) builds this file
 * as it is, with its own b_in_read/b_alloc. */
#include "b_asf.h"
#include "wma/wmadec.h"

static WMADecodeContext s_wma;          /* ~50 KiB, static like the rest of libwma */

typedef struct {
    b_asf_t asf;
    const uint8_t *rest;                /* superframes of the current object not yet started */
    uint32_t rest_len, sf_size;
    const uint8_t *sf;                  /* the superframe being decoded */
    uint32_t sf_len;
    int frames_left;                    /* of it */
    uint32_t out_pos, out_n;            /* samples in s_wma.frame_out to hand out */
    uint32_t errors;
} wma_t;

static wma_t *s;

uint32_t b_wma_errors(void) { return s ? s->errors + s->asf.errors : 0; }

int b_wma_open(void)
{
    asf_waveformatex_t wfx;
    const asf_cfg_t *c;
    int r;

    s = b_calloc(sizeof *s);
    if (!s)
        return -1;
    if ((r = b_asf_open(&s->asf)) != 0)
        return r;
    c = &s->asf.cfg;
    if (c->codec_id != ASF_WMAV1 && c->codec_id != ASF_WMAV2)
        return -2;
    memset(&wfx, 0, sizeof wfx);
    wfx.packet_size = c->packet_size;
    wfx.audiostream = c->stream;
    wfx.codec_id = c->codec_id;
    wfx.channels = c->channels;
    wfx.rate = c->rate;
    wfx.bitrate = c->bitrate;
    wfx.blockalign = c->block_align;
    wfx.bitspersample = c->bits;
    wfx.datalen = c->extra_len;
    memcpy(wfx.data, c->extra, c->extra_len);
    memset(&s_wma, 0, sizeof s_wma);
    if (wma_decode_init(&s_wma, &wfx) < 0)
        return -3;
    return 0;
}

uint32_t b_wma_read(int32_t *dst, uint32_t frames)
{
    uint32_t got = 0, ch = s->asf.cfg.channels;

    while (got < frames) {
        if (s->out_pos < s->out_n) {
            uint32_t n = s->out_n - s->out_pos;
            const fixed32 *l = &(*s_wma.frame_out)[0][s->out_pos], *r = &(*s_wma.frame_out)[ch > 1][s->out_pos];

            if (n > frames - got)
                n = frames - got;
            for (uint32_t i = 0; i < n; i++) {
                int32_t a = l[i], b = r[i];

                /* libwma's output is inverted (its IMDCT's sign): flip it back */
                a = a < -0x1FFFFFFF ? 0x1FFFFFFF : a > 0x20000000 ? -0x20000000 : -a;
                b = b < -0x1FFFFFFF ? 0x1FFFFFFF : b > 0x20000000 ? -0x20000000 : -b;
                if (ch > 1) {
                    dst[2 * (got + i)] = (int32_t)((uint32_t)a << 2);
                    dst[2 * (got + i) + 1] = (int32_t)((uint32_t)b << 2);
                } else {
                    dst[got + i] = (int32_t)((uint32_t)a << 2);
                }
            }
            s->out_pos += n;
            got += n;
            continue;
        }
        if (s->frames_left > 0) {
            int r = wma_decode_superframe_frame(&s_wma, s->sf, (int)s->sf_len);

            s->frames_left--;
            if (r < 0) {
                s->errors++;                 /* skip the rest of this superframe */
                s->frames_left = 0;
                continue;
            }
            s->out_pos = 0;
            s->out_n = (uint32_t)r;
            continue;
        }
        if (s->rest_len) {                   /* the object's next superframe */
            s->sf = s->rest;
            s->sf_len = s->rest_len < s->sf_size ? s->rest_len : s->sf_size;
            s->rest += s->sf_len;
            s->rest_len -= s->sf_len;
            wma_decode_superframe_init(&s_wma, s->sf, (int)s->sf_len);
            s->frames_left = s_wma.nb_frames;
            continue;
        }
        {
            const uint8_t *d;
            uint32_t len;

            if (!b_asf_object(&s->asf, &d, &len))
                break;                       /* end of the track */
            s->rest = d;
            s->rest_len = len;
            /* an object usually is one superframe (block_align bytes), but may hold several */
            s->sf_size = s->asf.cfg.block_align && len % s->asf.cfg.block_align == 0 ?
                         s->asf.cfg.block_align : len;
        }
    }
    return got;
}
