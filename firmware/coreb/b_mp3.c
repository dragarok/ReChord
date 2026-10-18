/* Core B: MPEG audio layer I/II/III with libmad (fixed point). Core A streams only
 * the frames; b_trk.trim drops the encoder + decoder delay (LAME header). */
#include "b_rt.h"
#include "mad.h"

/* libmad's two allocations (main data, IMDCT overlap) come from the arena */
void *malloc(size_t n) { return b_alloc(n); }
void *calloc(size_t k, size_t n) { return b_calloc(k * n); }
void free(void *p) { b_free(p); }

/* ---- MP3 (libmad, fixed point) -------------------------------------------- */

#define MP3_BUF 4096u                    /* > 2 frames of 320 kbit/s; refilled as frames are used */

typedef struct {
    struct mad_stream st;
    struct mad_frame fr;
    struct mad_synth sy;
    uint32_t pos;                        /* next sample of sy.pcm to hand out */
    uint32_t trim;                       /* decoded samples still to drop */
    uint32_t frames;                     /* frames decoded */
    int final;                           /* the buffer holds the end of the track (+ guard) */
    uint8_t buf[MP3_BUF + MAD_BUFFER_GUARD];
} mp3_t;

static mp3_t *s_mp3;

int b_mp3_open(void)
{
    s_mp3 = b_alloc(sizeof *s_mp3);
    if (!s_mp3)
        return -1;
    mad_stream_init(&s_mp3->st);
    mad_frame_init(&s_mp3->fr);
    mad_synth_init(&s_mp3->sy);
    s_mp3->pos = s_mp3->sy.pcm.length = 0;
    s_mp3->trim = b_trk.trim;
    s_mp3->frames = 0;
    s_mp3->final = 0;
    b_in_need = MP3_BUF;                     /* one refill of the frame buffer */
    return 0;
}

/* Keep the undecoded tail, append what the ring has of this track. */
static void mp3_fill(void)
{
    mp3_t *m = s_mp3;
    uint32_t keep = 0, n;

    if (m->st.buffer && m->st.next_frame) {
        keep = (uint32_t)(m->st.bufend - m->st.next_frame);
        memmove(m->buf, m->st.next_frame, keep);
    }
    n = b_in_avail();
    if (n > MP3_BUF - keep)
        n = MP3_BUF - keep;
    b_in_copy(m->buf + keep, n);
    keep += n;
    if (AS->in_rd == b_trk_end) {        /* the end: libmad wants zero guard bytes after it */
        memset(m->buf + keep, 0, MAD_BUFFER_GUARD);
        keep += MAD_BUFFER_GUARD;
        m->final = 1;
    }
    mad_stream_buffer(&m->st, m->buf, keep);
}

static int32_t mp3_word(mad_fixed_t v)
{
    if (v >= MAD_F_ONE)
        v = MAD_F_ONE - 1;
    else if (v < -MAD_F_ONE)
        v = -MAD_F_ONE;
    return (int32_t)((uint32_t)v << 3);  /* Q28 -> left-justified 32-bit word */
}

uint32_t b_mp3_read(int32_t *dst, uint32_t want)
{
    mp3_t *m = s_mp3;
    uint32_t got = 0, ch = b_trk.channels;

    while (got < want) {
        struct mad_pcm *pcm = &m->sy.pcm;

        if (m->pos < pcm->length) {
            uint32_t n = pcm->length - m->pos, r = pcm->channels > 1;

            if (m->trim) {
                if (n > m->trim)
                    n = m->trim;
                m->trim -= n;
                m->pos += n;
                continue;
            }
            if (n > want - got)
                n = want - got;
            {                            /* (locals: the stores to dst cannot alias m->pos) */
                const mad_fixed_t *l = &pcm->samples[0][m->pos], *rr = &pcm->samples[r][m->pos];
                int32_t *d = dst + got * ch;

                if (ch > 1) {
                    for (uint32_t i = 0; i < n; i++) {
                        d[0] = mp3_word(l[i]);
                        d[1] = mp3_word(rr[i]);
                        d += 2;
                    }
                } else {
                    for (uint32_t i = 0; i < n; i++)
                        d[i] = mp3_word(l[i]);
                }
            }
            m->pos += n;
            got += n;
            continue;
        }
        if (!m->st.buffer || m->st.error == MAD_ERROR_BUFLEN) {
            if (m->final)
                break;                   /* decoded everything */
            mp3_fill();
            m->st.error = MAD_ERROR_NONE;
        }
        if (mad_frame_decode(&m->fr, &m->st)) {
            if (m->st.error == MAD_ERROR_BADDATAPTR && b_trk.drop && m->frames < 16u) {
                /* a seek's pre-roll: this frame's bit reservoir lies before where the stream
                 * starts. Silence in its place keeps every later frame at its own sample
                 * (core B drops the pre-roll; audio.c seek_open) */
                mad_frame_mute(&m->fr);
                mad_synth_frame(&m->sy, &m->fr);
                m->pos = 0;
                m->frames++;
                continue;
            }
            if (MAD_RECOVERABLE(m->st.error) || m->st.error == MAD_ERROR_BUFLEN)
                continue;
            m->final = 1;                /* unrecoverable: end the track */
            m->st.error = MAD_ERROR_BUFLEN;
            continue;
        }
        mad_synth_frame(&m->sy, &m->fr);
        m->pos = 0;
        m->frames++;
    }
    return got;
}

