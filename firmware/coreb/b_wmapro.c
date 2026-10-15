/* Core B: WMA Pro (mono, stereo) with Rockbox's fixed-point libwmapro
 * (coreb/wma/README.rechord).
 *
 * Input: the ASF stream of b_asf.h, as media objects; each object is one WMA Pro
 * packet (block_align bytes), and decode_packet() takes it a frame at a time (a
 * frame may start in the previous packet: the decoder keeps those bits).
 * Output: the decoder's fixed point at Rockbox's sample depth 25 (1.0 = 2^25),
 * clipped to +-1.0 and left-justified in the 32-bit I2S word.
 *
 * Only b_rt.h is used, so the host reference (tests/wmaref.c) builds this file
 * as it is. */
#include "b_asf.h"
#include "wma/pro/wmaprodec.h"

typedef struct {
    b_asf_t asf;
    asf_waveformatex_t wfx;
    const uint8_t *data;                 /* rest of the current packet */
    uint32_t left;
    int32_t *dec[2];                     /* the decoder's output buffers */
    uint32_t out_pos, out_n;             /* frames in them to hand out */
    uint32_t errors;
} wmapro_t;

static wmapro_t *s;

uint32_t b_wmapro_errors(void) { return s ? s->errors + s->asf.errors : 0; }

int b_wmapro_open(void)
{
    const asf_cfg_t *c;
    int r;

    s = b_calloc(sizeof *s);
    if (!s)
        return -1;
    if ((r = b_asf_open(&s->asf)) != 0)
        return r;
    c = &s->asf.cfg;
    if (c->codec_id != ASF_WMAPRO || c->channels > 2)
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
    if (decode_init(&s->wfx) < 0)
        return -3;
    return 0;
}

static inline int32_t word(int32_t v)
{
    v = v > 0x01FFFFFF ? 0x01FFFFFF : v < -0x02000000 ? -0x02000000 : v;
    return (int32_t)((uint32_t)v << 6);
}

uint32_t b_wmapro_read(int32_t *dst, uint32_t frames)
{
    uint32_t got = 0, ch = s->asf.cfg.channels;

    while (got < frames) {
        if (s->out_pos < s->out_n) {
            uint32_t n = s->out_n - s->out_pos;
            const int32_t *l = s->dec[0] + s->out_pos, *r = s->dec[ch > 1] + s->out_pos;

            if (n > frames - got)
                n = frames - got;
            for (uint32_t i = 0; i < n; i++) {
                if (ch > 1) {
                    dst[2 * (got + i)] = word(l[i]);
                    dst[2 * (got + i) + 1] = word(r[i]);
                } else {
                    dst[got + i] = word(l[i]);
                }
            }
            s->out_pos += n;
            got += n;
            continue;
        }
        if (s->left) {                       /* the packet's next frame */
            int outlen = 0, r = decode_packet(&s->wfx, s->dec, &outlen, (void *)s->data, (int)s->left);

            if (r < 0) {
                s->errors++;                 /* drop the rest of this packet */
                s->left = 0;
                continue;
            }
            if (r == 0 && !outlen) {
                s->left = 0;                 /* no progress: next packet */
                continue;
            }
            s->data += r;
            s->left = (uint32_t)r < s->left ? s->left - (uint32_t)r : 0;
            s->out_pos = 0;
            s->out_n = (uint32_t)outlen / ch;
            continue;
        }
        {
            const uint8_t *d;
            uint32_t len;

            if (!b_asf_object(&s->asf, &d, &len))
                break;                       /* end of the track */
            s->data = d;
            s->left = len;
        }
    }
    return got;
}
