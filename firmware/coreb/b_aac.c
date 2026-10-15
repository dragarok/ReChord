/* Core B: AAC LC / HE-AAC (v1, v2) with FAAD2 (fixed point). Stream from core A:
 * u32 length + AudioSpecificConfig, then u32 length + one raw frame each. Output
 * is always stereo (b_trk.channels = 2): mono is doubled, PS makes its own two. */
#include "b_rt.h"
#include "neaacdec.h"

#define AAC_MAX_FRAME 6144u                  /* 6144 bits per channel; plenty for 2 */

static NeAACDecHandle s_h;
static uint8_t *s_pkt;
static int32_t *s_pcm;                       /* FAAD2's output buffer */
static uint32_t s_pos, s_n, s_ch, s_trim;

/* FAAD2 allocates through these (common.c is built with them renamed away) */
void *faad_malloc(size_t n) { return b_alloc(n); }
void faad_free(void *p) { b_free(p); }

int b_aac_open(void)
{
    uint8_t cfg[64];
    uint32_t len;
    unsigned long rate;
    unsigned char ch;
    NeAACDecConfigurationPtr c;

    if (b_in_read(&len, 4) != 4 || len == 0 || len > sizeof cfg || b_in_read(cfg, len) != len)
        return -1;
    s_h = NeAACDecOpen();
    s_pkt = b_alloc(AAC_MAX_FRAME + 8u);
    if (!s_h || !s_pkt)
        return -1;
    c = NeAACDecGetCurrentConfiguration(s_h);
    c->outputFormat = FAAD_FMT_24BIT;
    c->defObjectType = LC;
    NeAACDecSetConfiguration(s_h, c);
    if (NeAACDecInit2(s_h, cfg, len, &rate, &ch) < 0 || rate != b_trk.rate)
        return -2;                           /* core A predicted another output rate */
    s_pos = s_n = 0;
    s_trim = b_trk.trim;
    b_in_need = 4u + AAC_MAX_FRAME / 2u;     /* one packet (stereo frames stay under 1536 B) */
    return 0;
}

uint32_t b_aac_read(int32_t *dst, uint32_t frames)
{
    uint32_t got = 0;

    while (got < frames) {
        if (s_pos < s_n) {
            uint32_t n = s_n - s_pos;

            if (s_trim) {
                if (n > s_trim)
                    n = s_trim;
                s_trim -= n;
                s_pos += n;
                continue;
            }
            if (n > frames - got)
                n = frames - got;
            for (uint32_t i = 0; i < n; i++, s_pos++) {
                const int32_t *p = s_pcm + s_pos * s_ch;

                dst[(got + i) * 2] = (int32_t)((uint32_t)p[0] << 8);
                dst[(got + i) * 2 + 1] = (int32_t)((uint32_t)p[s_ch > 1] << 8);
            }
            got += n;
            continue;
        }
        {
            uint32_t len;
            NeAACDecFrameInfo fi;

            if (b_in_read(&len, 4) != 4)
                break;                       /* end of the track */
            if (len > AAC_MAX_FRAME) {
                AS->err = 0x50u;
                break;                       /* broken stream: end the track */
            }
            if (b_in_read(s_pkt, len) != len)
                break;
            s_pcm = NeAACDecDecode(s_h, &fi, s_pkt, len);
            if (fi.error || !s_pcm || !fi.channels) {
                s_n = s_pos = 0;             /* a bad frame is skipped */
                continue;
            }
            s_ch = fi.channels;
            s_n = (uint32_t)(fi.samples / fi.channels);
            s_pos = 0;
        }
    }
    return got;
}
