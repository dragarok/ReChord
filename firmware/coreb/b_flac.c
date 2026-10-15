/* Core B: FLAC with dr_flac (32-bit output = the I2S word format already). */
#include "b_rt.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_CRC
#define DRFLAC_ASSERT(x) ((void)0)
#define DRFLAC_MALLOC(sz)      ((void *)0)        /* every allocation goes to the arena */
#define DRFLAC_REALLOC(p, sz)  ((void *)0)
#define DRFLAC_FREE(p)         ((void)0)
#include "player/codecs/dr_flac/dr_flac.h"

static drflac *s_flac;

static size_t flac_read_cb(void *u, void *out, size_t n)
{
    return b_in_read(out, (uint32_t)n);
}

static drflac_bool32 flac_seek_cb(void *u, int offset, drflac_seek_origin origin)
{
    uint8_t sink[64];
    uint32_t cur = b_in_pos(), to;

    to = origin == DRFLAC_SEEK_SET ? (uint32_t)offset : cur + (uint32_t)offset;
    if (to < cur || to > b_trk_end - b_trk_start)
        return DRFLAC_FALSE;             /* forward only: the stream has no seek table to use */
    while (cur < to) {
        uint32_t k = to - cur > sizeof sink ? sizeof sink : to - cur;

        if (b_in_read(sink, k) != k)
            return DRFLAC_FALSE;
        cur += k;
    }
    return DRFLAC_TRUE;
}

static drflac_bool32 flac_tell_cb(void *u, drflac_int64 *pos)
{
    *pos = b_in_pos();
    return DRFLAC_TRUE;
}

static void *a_malloc(size_t n, void *u) { return b_alloc(n); }
static void *a_realloc(void *p, size_t n, void *u) { return b_realloc(p, n); }
static void a_free(void *p, void *u) { b_free(p); }
static const drflac_allocation_callbacks s_alloc = { NULL, a_malloc, a_realloc, a_free };

int b_flac_open(void)
{
    s_flac = drflac_open(flac_read_cb, flac_seek_cb, flac_tell_cb, NULL, &s_alloc);
    return s_flac && s_flac->channels <= 2 ? 0 : -1;
}

uint32_t b_flac_read(int32_t *dst, uint32_t frames)
{
    return (uint32_t)drflac_read_pcm_frames_s32(s_flac, frames, (drflac_int32 *)dst);
}
