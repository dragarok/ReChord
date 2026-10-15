/* Core B: Ogg Vorbis with Tremor (integer, Xiph). Core A streams the whole file;
 * vorbisfile reads it non-seekably, trims the last page by its granule position
 * (gapless), and hands out 16-bit samples. */
#include "b_rt.h"
#include "ivorbisfile.h"

/* Tremor and libogg allocate through the C names */
void *malloc(size_t n) { return b_alloc(n); }
void *calloc(size_t k, size_t n) { return b_calloc(k * n); }
void *realloc(void *p, size_t n) { return b_realloc(p, n); }
void free(void *p) { b_free(p); }

static OggVorbis_File *s_vf;
static int16_t *s_pcm;
static uint32_t s_pos, s_n;

#define PCM_BYTES 4096u

static size_t rd(void *ptr, size_t size, size_t nmemb, void *src)
{
    return b_in_read(ptr, (uint32_t)(size * nmemb)) / (size ? size : 1);
}

int b_vorbis_open(void)
{
    ov_callbacks cb = { rd, NULL, NULL, NULL };
    vorbis_info *vi;

    s_vf = b_calloc(sizeof *s_vf);
    s_pcm = b_alloc(PCM_BYTES);
    /* the datasource is unused but must not be NULL: vorbisfile reads nothing from a NULL one */
    if (!s_vf || !s_pcm || ov_open_callbacks((void *)1, s_vf, NULL, 0, cb) < 0)
        return -1;
    vi = ov_info(s_vf, -1);
    if (!vi || (uint32_t)vi->rate != b_trk.rate || (uint32_t)vi->channels != b_trk.channels)
        return -2;
    s_pos = s_n = 0;
    b_in_need = 0x2000u;                     /* a page or two */
    return 0;
}

uint32_t b_vorbis_read(int32_t *dst, uint32_t frames)
{
    uint32_t got = 0, ch = b_trk.channels;

    while (got < frames) {
        if (s_pos < s_n) {
            uint32_t n = s_n - s_pos;

            if (n > frames - got)
                n = frames - got;
            for (uint32_t i = 0; i < n * ch; i++)
                dst[got * ch + i] = (int32_t)((uint32_t)(uint16_t)s_pcm[s_pos * ch + i] << 16);
            s_pos += n;
            got += n;
            continue;
        }
        {
            int bs;
            long r = ov_read(s_vf, (char *)s_pcm, (int)PCM_BYTES, &bs);

            if (r == OV_HOLE)
                continue;                    /* a gap in the data: carry on */
            if (r <= 0)
                break;                       /* end of the track (or an error) */
            s_n = (uint32_t)r / (2u * ch);
            s_pos = 0;
        }
    }
    return got;
}
