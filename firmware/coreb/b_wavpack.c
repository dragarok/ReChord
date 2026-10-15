/* Core B: WavPack (lossless, hybrid lossy without a .wvc; integer and float) with
 * the official library, decoder only, read non-seekably over the ring. Core A
 * streams the whole file. */
#include "b_rt.h"
#include "wavpack.h"

void *malloc(size_t n) { return b_alloc(n); }
void *calloc(size_t k, size_t n) { return b_calloc(k * n); }
void *realloc(void *p, size_t n) { return b_realloc(p, n); }
void free(void *p) { b_free(p); }

static WavpackContext *s_wpc;
static int s_back = -1;                          /* push_back_byte */
static uint32_t s_shift, s_float;

static int32_t rd(void *id, void *data, int32_t n)
{
    uint8_t *d = data;
    int32_t got = 0;

    if (n > 0 && s_back >= 0) {
        d[got++] = (uint8_t)s_back;
        s_back = -1;
    }
    return got + (int32_t)b_in_read(d + got, (uint32_t)(n - got));
}
static int64_t pos(void *id) { return (int64_t)b_in_pos() - (s_back >= 0); }
static int no_seek_abs(void *id, int64_t p) { return -1; }
static int no_seek_rel(void *id, int64_t d, int m) { return -1; }
static int push_back(void *id, int c) { s_back = c; return c; }
static int64_t length(void *id) { return (int64_t)(b_trk_end - b_trk_start); }
static int can_seek(void *id) { return 0; }

static WavpackStreamReader64 s_reader = {
    rd, NULL, pos, no_seek_abs, no_seek_rel, push_back, length, can_seek, NULL, NULL
};

/* IEEE single (normalized to +-1) -> Q31 word by its bits; same as b_pcm.c */
static int32_t float_word(uint32_t f)
{
    uint32_t e = f >> 23 & 0xFFu, m = (f & 0x7FFFFFu) | 0x800000u;
    int32_t v;

    if (e >= 127u)
        v = 0x7FFFFFFF;
    else if (e < 127u - 31u)
        v = 0;
    else
        v = (int32_t)(((uint64_t)m << 7) >> (126u - e));
    if ((f >> 31) && v)
        v = v == 0x7FFFFFFF ? (int32_t)0x80000000u : -v;
    return v;
}

int b_wavpack_open(void)
{
    char err[80];

    s_back = -1;
    s_wpc = WavpackOpenFileInputEx64(&s_reader, (void *)1, NULL, err, OPEN_2CH_MAX | OPEN_NORMALIZE, 0);
    if (!s_wpc)
        return -1;
    if ((uint32_t)WavpackGetSampleRate(s_wpc) != b_trk.rate ||
        (uint32_t)WavpackGetReducedChannels(s_wpc) != b_trk.channels)
        return -2;
    s_float = (WavpackGetMode(s_wpc) & MODE_FLOAT) != 0;
    s_shift = 32u - 8u * (uint32_t)WavpackGetBytesPerSample(s_wpc);
    b_in_need = 0x4000u;                         /* a block or two */
    return 0;
}

uint32_t b_wavpack_read(int32_t *dst, uint32_t frames)
{
    uint32_t n = WavpackUnpackSamples(s_wpc, dst, frames), ch = b_trk.channels;

    for (uint32_t i = 0; i < n * ch; i++)
        dst[i] = s_float ? float_word((uint32_t)dst[i]) : (int32_t)((uint32_t)dst[i] << s_shift);
    return n;
}
