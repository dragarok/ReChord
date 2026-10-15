/* Core B: Ogg Opus with libopus (fixed point) + opusfile. Core A streams the whole
 * file; opusfile applies the pre-skip and trims the last page by its granule
 * position (gapless), and mixes anything beyond stereo down to stereo. Output: 48 kHz. */
#include "b_rt.h"
#include "opusfile.h"

void *malloc(size_t n) { return b_alloc(n); }
void *calloc(size_t k, size_t n) { return b_calloc(k * n); }
void *realloc(void *p, size_t n) { return b_realloc(p, n); }
void free(void *p) { b_free(p); }

static OggOpusFile *s_of;
static opus_int16 *s_pcm;
static uint32_t s_pos, s_n;

#define PCM_FRAMES 960u                          /* 20 ms */

static int rd(void *src, unsigned char *ptr, int n)
{
    return (int)b_in_read(ptr, (uint32_t)n);
}

int b_opus_open(void)
{
    static const OpusFileCallbacks cb = { rd, NULL, NULL, NULL };
    int err;

    s_pcm = b_alloc(PCM_FRAMES * 2u * sizeof *s_pcm);
    if (!s_pcm || b_trk.rate != 48000u || b_trk.channels != 2u)
        return -1;
    s_of = op_open_callbacks((void *)1, &cb, NULL, 0, &err);
    if (!s_of)
        return -2;
    s_pos = s_n = 0;
    b_in_need = 0x2000u;
    return 0;
}

uint32_t b_opus_read(int32_t *dst, uint32_t frames)
{
    uint32_t got = 0;

    while (got < frames) {
        if (s_pos < s_n) {
            uint32_t n = s_n - s_pos;

            if (n > frames - got)
                n = frames - got;
            for (uint32_t i = 0; i < n * 2u; i++)
                dst[got * 2u + i] = (int32_t)((uint32_t)(uint16_t)s_pcm[s_pos * 2u + i] << 16);
            s_pos += n;
            got += n;
            continue;
        }
        {
            int r = op_read_stereo(s_of, s_pcm, (int)(PCM_FRAMES * 2u));

            if (r == OP_HOLE)
                continue;
            if (r <= 0)
                break;
            s_n = (uint32_t)r;
            s_pos = 0;
        }
    }
    return got;
}
