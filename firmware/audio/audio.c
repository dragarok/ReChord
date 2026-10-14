/*
 * audio.c -- core A side of the audio engine (contract: audio_shared.h).
 *
 * Core A never touches a sample. Per 10 ms tick it only tops up the input ring
 * from the file (FLAC: synthesized "fLaC" + STREAMINFO, then the frames, so
 * cover art, padding and tags are never read; WAV: the raw data chunk), opens
 * the next playlist entry when a file ends (gapless: its bytes follow in the
 * same ring) and starts/stops the I2S DMA. Core B decodes; the DMA's circular
 * LLI chain feeds I2S1 from the PCM ring without CPU help.
 *
 * I2S/DMA programming follows RECHO 0.9.2 as traced in QEMU (core-A DMA, I2S1
 * TXDR, CTL 0x18909124, CFG 0x840/0x1007 = handshake 2, I2S DMACR 0x110).
 */
#include <stdint.h>
#include <string.h>

#include "audio.h"
#include "audio_shared.h"
#include "drivers/clock.h"
#include "drivers/dac_cs43131.h"
#include "services/fs.h"

extern const uint8_t coreb_image[], coreb_image_end[];

#define REG(a)      (*(volatile uint32_t *)(a))
#define CRU_SOFTRST3 0x401800CCu
#define CAL_CORE_RST (1u << 2)

#define DMA         0x60000000u
#define DMA_CH      5u
#define DMA_CHR(r)  REG(DMA + DMA_CH * 0x58u + (r))
#define DMA_CFGREG  REG(DMA + 0x398u)
#define DMA_CHEN    REG(DMA + 0x3A0u)
#define I2S1        0x40030000u
#define I2S_DMACR   REG(I2S1 + 0x10u)
#define I2S_TXDR    (I2S1 + 0x24u)

#define LLI_CTL_L   0x18909124u           /* M2P, 32-bit, src inc, burst 8, LLP src+dst */
#define LLI_CTL_H   (AS_BLOCK_FRAMES * 2u)  /* block size in words */

/* ---- playlist: a directory and an index into its listing ---------------- */

#define PATH_MAX_A 160
static char s_dir[PATH_MAX_A];
static uint32_t s_next_index;              /* next directory entry to try */
static char s_name[FS_NAME_MAX];           /* entry being streamed */

/* ---- streaming state ----------------------------------------------------- */

static fs_file_t *s_file;
static uint8_t s_hdr[42];
static uint32_t s_hdr_len, s_hdr_pos;
static uint32_t s_left;                    /* file bytes still to stream */
static uint32_t s_data_off;                /* file offset of the first streamed byte */
static uint32_t s_skip_left;               /* alignment filler still to write */
static uint32_t s_seq;
static int s_b_up, s_dac_up, s_dma_on, s_playing, s_paused;
static uint32_t s_rate;
static audio_track_t s_tracks[AS_QUEUE];   /* titles/durations by seq, for the UI */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static int ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');

    if (!dot)
        return 0;
    for (dot++; *dot && *ext; dot++, ext++)
        if ((*dot | 0x20) != *ext)
            return 0;
    return !*dot && !*ext;
}

static int playable(const char *name)
{
    return ext_is(name, "flac") || ext_is(name, "wav");
}

/* FLAC: STREAMINFO -> track, synthesized header in s_hdr, file left at the first frame. */
static int open_flac(fs_file_t *f, as_track_t *t)
{
    uint8_t b[4], si[34];
    uint32_t pos = 4;
    int last = 0, have = 0;

    if (fs_read(f, b, 4) != 4 || memcmp(b, "fLaC", 4))
        return -1;
    while (!last) {
        uint32_t len;

        if (fs_read(f, b, 4) != 4)
            return -1;
        last = b[0] >> 7;
        len = (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        pos += 4;
        if ((b[0] & 0x7F) == 0 && len == 34) {
            if (fs_read(f, si, 34) != 34)
                return -1;
            have = 1;
        } else if (fs_seek(f, (long)(pos + len)) < 0) {   /* skip PICTURE, SEEKTABLE, ... unread */
            return -1;
        }
        pos += len;
    }
    if (!have)
        return -1;
    t->codec = AS_CODEC_FLAC;
    t->rate = be32(si + 10) >> 12;
    t->channels = ((si[12] >> 1) & 7u) + 1u;
    t->bits = ((((uint32_t)si[12] & 1u) << 4) | (si[13] >> 4)) + 1u;
    t->frames = (si[13] & 0x0Fu) ? 0u : be32(si + 14);   /* > 2^32 frames: unknown */
    memcpy(s_hdr, "fLaC", 4);
    s_hdr[4] = 0x80;                       /* last block, STREAMINFO */
    s_hdr[5] = 0; s_hdr[6] = 0; s_hdr[7] = 34;
    memcpy(s_hdr + 8, si, 34);
    s_hdr_len = 42;
    s_left = (uint32_t)fs_file_size(f) - pos;
    s_data_off = pos;
    t->bytes = s_hdr_len + s_left;
    return 0;
}

/* WAV: fmt + data chunk -> track; raw PCM is streamed as is. */
static int open_wav(fs_file_t *f, as_track_t *t)
{
    uint8_t b[24];
    uint32_t pos = 12;
    int fmt = 0;

    if (fs_read(f, b, 12) != 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4))
        return -1;
    for (;;) {
        uint32_t len;

        if (fs_read(f, b, 8) != 8)
            return -1;
        len = le32(b + 4);
        pos += 8;
        if (!memcmp(b, "fmt ", 4)) {
            if (fs_read(f, b, 16) != 16)
                return -1;
            t->channels = le16(b + 2);
            t->rate = le32(b + 4);
            t->bits = le16(b + 14);
            fmt = 1;
        } else if (!memcmp(b, "data", 4)) {
            break;
        }
        pos += len + (len & 1u);
        if (fs_seek(f, (long)pos) < 0)
            return -1;
    }
    if (!fmt || !t->channels)
        return -1;
    t->codec = AS_CODEC_WAV;
    s_hdr_len = 0;
    s_data_off = pos;
    s_left = (uint32_t)fs_file_size(f) - pos;
    {
        uint32_t len = le32(b + 4);
        if (len < s_left)
            s_left = len;
    }
    t->frames = s_left / (t->channels * (t->bits / 8u));
    t->bytes = s_left;
    return 0;
}

/* Open the next playable entry of the playlist directory and queue it for B. */
static int open_next(void)
{
    fs_dirent_t ent;
    uint32_t got, total;
    char path[PATH_MAX_A + FS_NAME_MAX];

    for (;;) {
        as_track_t t;
        size_t dl;

        if (fs_list_dir(s_dir, s_next_index, &ent, 1, &got, &total) != 0 || got == 0)
            return -1;
        s_next_index++;
        if (ent.is_dir || !playable(ent.name))
            continue;
        dl = strlen(s_dir);
        memcpy(path, s_dir, dl);
        if (!dl || path[dl - 1] != '/')
            path[dl++] = '/';
        strcpy(path + dl, ent.name);
        s_file = fs_open_read(path);
        if (!s_file)
            continue;
        memset(&t, 0, sizeof t);
        if ((ext_is(ent.name, "flac") ? open_flac(s_file, &t) : open_wav(s_file, &t)) != 0) {
            fs_close(s_file);
            s_file = NULL;
            continue;
        }
        t.seq = ++s_seq;
        /* file byte p lands at ring offset in_wr + skip + hdr + (p - start): make that
         * congruent to p mod 4, so whole sectors can be read straight into the ring */
        t.skip = (s_data_off - (AS->in_wr + s_hdr_len)) & 3u;
        t.bytes += t.skip;
        s_skip_left = t.skip;
        s_hdr_pos = 0;
        strncpy(s_name, ent.name, sizeof s_name - 1);
        {
            audio_track_t *u = &s_tracks[t.seq % AS_QUEUE];

            u->seq = t.seq;
            u->rate = t.rate;
            u->bits = t.bits;
            u->channels = t.channels;
            u->frames = t.frames;
            u->codec = t.codec;
            u->index = s_next_index - 1u;
            strncpy(u->name, ent.name, sizeof u->name - 1);
            u->name[sizeof u->name - 1] = 0;
        }
        AS->q[AS->q_wr % AS_QUEUE] = t;
        AS->q_wr++;
        if (!s_rate)
            s_rate = t.rate;
        return 0;
    }
}

/* Top up the input ring: header bytes, then file data, then the next file. */
static void refill(void)
{
    uint8_t *ring = (uint8_t *)AS_IN_RING;

    for (int guard = 0; guard < 8; guard++) {
        uint32_t used = AS->in_wr - AS->in_rd, free = AS_IN_SIZE - used;
        uint32_t off = AS->in_wr % AS_IN_SIZE, k = AS_IN_SIZE - off;

        if (!s_file) {
            if (AS->in_closed || AS->q_wr - AS->q_rd >= AS_QUEUE - 1u)
                return;
            if (open_next() != 0) {
                AS->in_closed = 1;           /* end of the playlist */
                return;
            }
            continue;
        }
        if (free < 512u)
            return;
        if (k > free)
            k = free;
        if (s_skip_left) {
            uint32_t n = s_skip_left < k ? s_skip_left : k;

            memset(ring + off, 0, n);
            s_skip_left -= n;
            AS->in_wr += n;
            continue;
        }
        if (s_hdr_pos < s_hdr_len) {
            uint32_t n = s_hdr_len - s_hdr_pos;

            if (n > k)
                n = k;
            memcpy(ring + off, s_hdr + s_hdr_pos, n);
            s_hdr_pos += n;
            AS->in_wr += n;
            continue;
        }
        if (s_left) {
            long n;

            if (k > s_left)
                k = s_left;
            if (k > 16384u)
                k = 16384u;                 /* bound the time spent per tick */
            n = fs_read(s_file, ring + off, k);
            if (n <= 0) {                   /* read error: B was promised the bytes, */
                memset(ring + off, 0, k);   /* so the rest of the track reads as zeros */
                n = (long)k;
            }
            {
                s_left -= (uint32_t)n;
                AS->in_wr += (uint32_t)n;
            }
            if (s_left)
                continue;
        }
        fs_close(s_file);
        s_file = NULL;
    }
}

/* ---- core B ------------------------------------------------------------- */

static int b_start(void)
{
    uint32_t n = (uint32_t)(coreb_image_end - coreb_image);

    if (s_b_up)
        return 0;
    if (n > AS_B_IMAGE_MAX)
        return -1;
    REG(CRU_SOFTRST3) = (CAL_CORE_RST << 16) | CAL_CORE_RST;     /* hold B in reset */
    memcpy((void *)AS_B_RAM, coreb_image, n);
    memset((void *)AS_SHARED, 0, sizeof(audio_shared_t));
    AS->magic = AS_MAGIC;
    AS->version = AS_VERSION;
    AS->b_hz = clock_core_b_hz();
    AS->b_div = AS->b_div_req = clock_pll_hz() / clock_core_b_hz();
    REG(CRU_SOFTRST3) = CAL_CORE_RST << 16;                      /* release */
    for (uint32_t t0 = time_ms(); AS->state != AS_B_READY; ) {
        if (time_ms() - t0 > 200u)
            return -2;
        cpu_wait_tick();
    }
    s_b_up = 1;
    return 0;
}

/* ---- DMA + I2S ------------------------------------------------------------ */

static void dma_prepare(void)
{
    volatile uint32_t *lli = (volatile uint32_t *)AS_LLI;

    for (uint32_t i = 0; i < AS_PCM_BLOCKS; i++) {
        volatile uint32_t *l = lli + i * 7u;

        l[0] = AS_PCM_RING + i * AS_BLOCK_BYTES;
        l[1] = I2S_TXDR;
        l[2] = AS_LLI + ((i + 1u) % AS_PCM_BLOCKS) * 28u;
        l[3] = LLI_CTL_L;
        l[4] = LLI_CTL_H;
        l[5] = l[6] = 0;
    }
}

static void dma_start(void)
{
    DMA_CFGREG = 1;
    DMA_CHR(0x00) = AS_PCM_RING;
    DMA_CHR(0x08) = I2S_TXDR;
    DMA_CHR(0x10) = AS_LLI;                  /* first LLI is fetched at enable */
    DMA_CHR(0x18) = LLI_CTL_L;
    DMA_CHR(0x1C) = LLI_CTL_H;
    DMA_CHR(0x40) = 0x840u;
    DMA_CHR(0x44) = 0x1007u;                 /* dest handshake 2 = I2S1 TX */
    DMA_CHEN = (1u << (DMA_CH + 8u)) | (1u << DMA_CH);
    I2S_DMACR = 0x110u;                      /* TX DMA on, level 16 */
    dac_i2s_start();
    s_dma_on = 1;
}

static void dma_stop(void)
{
    dac_i2s_stop();
    I2S_DMACR = 0;
    DMA_CHEN = 1u << (DMA_CH + 8u);
    s_dma_on = 0;
}

/* ---- volume: CS43131 PCM volume A/B (MAP 0x090001/0x090002), 0.5 dB steps,
 * both chips; -40 dB at power-on like RECHO (the chip's own default is 0 dB,
 * full scale into the headphones). ------------------------------------------ */

#define VOL_MIN_DB (-60)
static int s_vol_db = -40;

static void dac_volume(void)
{
    uint8_t v = (uint8_t)(-s_vol_db * 2);

    for (int chan = 0; chan < 2; chan++) {
        dac_write_reg(chan, 0x090001u, v);
        dac_write_reg(chan, 0x090002u, v);
    }
}

int audio_set_volume_db(int db)
{
    s_vol_db = db < VOL_MIN_DB ? VOL_MIN_DB : db > 0 ? 0 : db;
    if (s_dac_up)
        dac_volume();
    return s_vol_db;
}

int audio_get_volume_db(void)
{
    return s_vol_db;
}

/* ---- API ---------------------------------------------------------------- */

static void halt(void)
{
    if (s_dma_on)
        dma_stop();
    if (s_file) {
        fs_close(s_file);
        s_file = NULL;
    }
    if (s_b_up) {
        AS->run = 0;
        AS->cmd = AS_CMD_STOP;
        for (uint32_t t0 = time_ms(); AS->cmd && time_ms() - t0 < 200u; )
            cpu_wait_tick();
    }
    s_playing = 0;
    s_paused = 0;
}

int audio_play_dir(const char *dir, uint32_t index)
{
    int r;

    halt();
    if ((r = b_start()) != 0)
        return r;
    strncpy(s_dir, dir, sizeof s_dir - 1);
    s_next_index = index;
    s_rate = 0;
    AS->in_wr = AS->in_rd = 0;
    AS->q_wr = AS->q_rd = 0;
    AS->in_closed = 0;
    AS->pcm_wr = AS->pcm_played = AS->pcm_end = 0;
    AS->underruns = AS->err = 0;
    AS->cur_seq = AS->done_seq = 0;
    dma_prepare();
    refill();
    if (AS->q_wr == 0)
        return -3;                           /* nothing playable */
    if (!s_dac_up) {
        if (dac_init(s_rate) != 0)
            return -4;
        s_dac_up = 1;
    }
    dac_volume();                            /* before the first sample reaches the DAC */
    AS->run = 1;
    s_playing = 1;
    return 0;
}

void audio_stop(void)
{
    halt();
}

int audio_skip(int dir)
{
    audio_status_t st;
    fs_dirent_t ent;
    uint32_t got, total, idx;

    audio_status(&st);
    if (!st.seq)
        return -3;
    idx = st.track.index;
    if (dir > 0)
        return audio_play_dir(s_dir, idx + 1u);
    if (st.frames <= 3u * st.track.rate) {          /* near the start: the previous playable entry */
        for (uint32_t j = idx; j-- > 0; ) {
            if (fs_list_dir(s_dir, j, &ent, 1, &got, &total) == 0 && got && !ent.is_dir && playable(ent.name))
                return audio_play_dir(s_dir, j);
        }
    }
    return audio_play_dir(s_dir, idx);              /* restart this one */
}

/* Pause: stop the I2S transfer; the DMA then stalls on its handshake and keeps
 * its place, core B stops once the ring is full. Resume restarts I2S. */

int audio_pause(int on)
{
    if (!s_playing || !s_dma_on || on == s_paused)
        return s_paused;
    if (on)
        dac_i2s_stop();
    else
        dac_i2s_start();
    s_paused = on;
    return s_paused;
}

int audio_paused(void)
{
    return s_playing && s_paused;
}

void audio_poll(void)
{
    if (!s_playing)
        return;
    if (AS->b_div_req != AS->b_div) {        /* core B asked for another clock */
        clock_set_core_b_div(AS->b_div_req);
        AS->b_hz = clock_core_b_hz();
        AS->b_div = AS->b_div_req;
    }
    refill();
    /* start output once B has filled the ring (or the whole stream is shorter) */
    if (!s_dma_on && (AS->pcm_wr >= AS_PCM_BLOCKS || (AS->pcm_end && AS->pcm_wr >= AS->pcm_end)))
        dma_start();
    if (AS->state == AS_B_DRAINED || AS->state == AS_B_ERROR)
        halt();
}

int audio_status(audio_status_t *st)
{
    /* before core B opens the first track, report that track */
    uint32_t seq = s_b_up && AS->cur_seq ? AS->cur_seq : (s_seq ? s_seq - (uint32_t)(AS->q_wr - 1u) : 0);
    const audio_track_t *t = &s_tracks[seq % AS_QUEUE];

    memset(st, 0, sizeof *st);
    st->playing = s_playing;
    st->seq = seq;
    if (s_b_up && seq && t->seq == seq) {
        st->track = *t;
        /* decoded minus what still waits in the ring */
        {
            uint32_t buffered = (AS->pcm_wr - AS->pcm_played) * AS_BLOCK_FRAMES;

            st->frames = AS->cur_frames > buffered ? AS->cur_frames - buffered : 0;
        }
    }
    st->b_load_pm = s_b_up ? AS->b_load_pm : 0;
    st->b_hz = s_b_up ? AS->b_hz : 0;
    st->underruns = s_b_up ? AS->underruns : 0;
    st->err = s_b_up ? AS->err : 0;
    return 0;
}
