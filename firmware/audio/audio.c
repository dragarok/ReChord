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
#include "ape_info.h"
#include "mp3_info.h"
#include "mp4.h"
#include "asf.h"
#include "drivers/clock.h"
#include "drivers/dac_cs43131.h"
#include "drivers/usb_out.h"
#include "services/fs.h"
#include "services/log.h"
#include "services/modules.h"
#include "services/dirlist.h"
#include "services/queue.h"
#include "services/bootcrumb.h"


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

static char s_name[FS_NAME_MAX];           /* entry being streamed */

/* ---- streaming state ----------------------------------------------------- */

static fs_file_t *s_file;
static uint8_t s_hdr[72];
static uint32_t s_hdr_len, s_hdr_pos;
static uint32_t s_left;                    /* file bytes still to stream */
static uint32_t s_data_off;                /* file offset of the first streamed byte */
static uint32_t s_skip_left;               /* alignment filler still to write */
static uint32_t s_seq;
static int s_b_up, s_dac_up, s_dma_on, s_playing, s_paused;
static int s_usb;                    /* the output is USB audio (usb_out.h), not the jack */
static uint32_t s_underruns, s_underrun_log;   /* B's count when last seen; ms of the last log */
static uint32_t s_rate, s_rate_changes;
static audio_track_t s_tracks[AS_QUEUE];   /* titles/durations by seq, for the UI */
static uint32_t s_sv_ticks, s_sv_ticks_at, s_sv_pcm, s_sv_pcm_at, s_sv_done, s_sv_fails, s_sv_aborts;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }


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

/* WAV: fmt + data chunk -> track; raw PCM is streamed as is. PCM 8..32 bit,
 * IEEE float 32 (core B converts), WAVE_FORMAT_EXTENSIBLE of either; a data
 * length of 0 or ~0 (written by a recorder that never finished) = to the end. */
static int open_wav(fs_file_t *f, as_track_t *t)
{
    uint8_t b[40];
    uint32_t pos = 12, size = (uint32_t)fs_file_size(f), fmt = 0, len;

    if (fs_read(f, b, 12) != 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4))
        return -1;
    for (;;) {
        if (fs_read(f, b, 8) != 8)
            return -1;
        len = le32(b + 4);
        pos += 8;
        if (!memcmp(b, "fmt ", 4)) {
            uint32_t n = len < sizeof b ? len : sizeof b;

            if (n < 16 || fs_read(f, b, n) != (long)n)
                return -1;
            fmt = le16(b);
            if (fmt == 0xFFFEu && n >= 26)
                fmt = le16(b + 24);                  /* EXTENSIBLE: the subformat GUID's first word */
            t->channels = le16(b + 2);
            t->rate = le32(b + 4);
            t->bits = le16(b + 14);
        } else if (!memcmp(b, "data", 4)) {
            break;
        }
        pos += len + (len & 1u);
        if (pos >= size || fs_seek(f, (long)pos) < 0)
            return -1;
    }
    if (!(fmt == 1u || (fmt == 3u && t->bits == 32u)) || !t->channels || t->channels > 2 || !t->rate)
        return -1;
    t->codec = fmt == 3u ? AS_CODEC_WAVF : AS_CODEC_WAV;
    s_hdr_len = 0;
    s_data_off = pos;
    s_left = size - pos;
    if (len && len != 0xFFFFFFFFu && len < s_left)
        s_left = len;
    s_left -= s_left % (t->channels * (t->bits / 8u));
    t->frames = s_left / (t->channels * (t->bits / 8u));
    t->bytes = s_left;
    return 0;
}

/* AIFF / AIFC: COMM (channels, frames, bits, 80-bit float rate), SSND (offset,
 * then big-endian PCM); AIFC 'NONE' or 'sowt' (little-endian). */
static int open_aiff(fs_file_t *f, as_track_t *t)
{
    uint8_t b[26];
    uint32_t pos = 12, size = (uint32_t)fs_file_size(f), len, frames = 0;
    int comm = 0, aifc, sowt = 0;

    if (fs_read(f, b, 12) != 12 || memcmp(b, "FORM", 4) || (memcmp(b + 8, "AIFF", 4) && memcmp(b + 8, "AIFC", 4)))
        return -1;
    aifc = !memcmp(b + 8, "AIFC", 4);
    for (;;) {
        if (fs_read(f, b, 8) != 8)
            return -1;
        len = be32(b + 4);
        pos += 8;
        if (!memcmp(b, "COMM", 4)) {
            uint32_t e, m;

            if (len < 18 || fs_read(f, b, len < sizeof b ? len : sizeof b) < 18)
                return -1;
            t->channels = (uint32_t)b[0] << 8 | b[1];
            frames = be32(b + 2);
            t->bits = (uint32_t)b[6] << 8 | b[7];
            e = ((uint32_t)b[8] << 8 | b[9]) & 0x7FFFu;    /* 80-bit extended: rate */
            m = be32(b + 10);
            t->rate = e >= 16383u && e < 16383u + 32u ? m >> (16383u + 31u - e) : 0;
            if (aifc && len >= 22) {
                if (!memcmp(b + 18, "sowt", 4))
                    sowt = 1;
                else if (memcmp(b + 18, "NONE", 4))
                    return -1;                       /* compressed AIFC */
            }
            comm = 1;
        } else if (!memcmp(b, "SSND", 4)) {
            if (fs_read(f, b, 8) != 8)
                return -1;
            pos += 8 + be32(b);                     /* offset (block alignment) */
            break;
        }
        pos += len + (len & 1u);
        if (pos >= size || fs_seek(f, (long)pos) < 0)
            return -1;
    }
    if (!comm || !t->channels || t->channels > 2 || !t->rate || fs_seek(f, (long)pos) < 0)
        return -1;
    t->bits = (t->bits + 7u) & ~7u;                  /* e.g. 20-bit samples in 3 bytes */
    t->codec = sowt ? AS_CODEC_AIFF_LE : AS_CODEC_AIFF;
    s_hdr_len = 0;
    s_data_off = pos;
    s_left = size - pos;
    if (frames && frames * t->channels * (t->bits / 8u) < s_left)
        s_left = frames * t->channels * (t->bits / 8u);
    s_left -= s_left % (t->channels * (t->bits / 8u));
    t->frames = s_left / (t->channels * (t->bits / 8u));
    t->bytes = s_left;
    return 0;
}

/* MP3: only the MPEG frames are streamed (tags and the Xing/Info frame never leave
 * the eMMC); the LAME delay/padding become the trim and the frame count. */
static uint32_t mp3_rd(void *u, uint32_t off, void *buf, uint32_t n)
{
    long r;

    if (fs_seek((fs_file_t *)u, (long)off) < 0)
        return 0;
    r = fs_read((fs_file_t *)u, buf, n);
    return r < 0 ? 0u : (uint32_t)r;
}

static uint32_t s_kbps;

static int open_mp3(fs_file_t *f, as_track_t *t)
{
    mp3_info_t mi;

    if (mp3_info(mp3_rd, f, (uint32_t)fs_file_size(f), &mi) != 0 || fs_seek(f, (long)mi.data_off) < 0)
        return -1;
    t->codec = AS_CODEC_MP3;
    t->rate = mi.rate;
    t->channels = mi.channels;
    t->bits = 16;
    t->frames = mi.frames;
    t->trim = mi.skip;
    s_kbps = mi.bitrate;
    s_hdr_len = 0;
    s_data_off = mi.data_off;
    s_left = mi.data_end - mi.data_off;
    t->bytes = s_left;
    return 0;
}

/* MP4/M4A (AAC, ALAC): core B gets "u32 length + codec config", then
 * "u32 length + frame" for every sample, read through the sample tables. The
 * tables are read on a second handle, marked at them, so neither handle walks
 * the cluster chain from the start of the file. */
static int s_mp4;                          /* the open track is MP4 */
static fs_file_t *s_tab;                   /* its sample-table handle */
static mp4_info_t s_mi;
static mp4_cur_t s_cur;
static uint32_t s_fpos;                    /* where s_file is */
static uint32_t s_stream_left;             /* MP4: bytes promised to B not yet written */

static int open_m4a(fs_file_t *f, const fs_ref_t *ref, as_track_t *t)
{
    uint32_t tables;

    if (mp4_info(mp3_rd, f, (uint32_t)fs_file_size(f), &s_mi) != 0)
        return -1;
    s_tab = fs_open_ref(ref);
    if (!s_tab)
        return -1;
    tables = s_mi.stsz_off < s_mi.stco_off ? s_mi.stsz_off : s_mi.stco_off;
    if (s_mi.stsc_off < tables)
        tables = s_mi.stsc_off;
    fs_seek(s_tab, (long)tables);
    fs_mark(s_tab);
    mp4_cursor(&s_cur);
    t->codec = s_mi.codec == MP4_AAC ? AS_CODEC_AAC : AS_CODEC_ALAC;
    t->rate = s_mi.rate;
    t->channels = s_mi.channels;
    t->bits = s_mi.bits;
    t->frames = s_mi.frames;
    t->trim = s_mi.trim;
    s_hdr[0] = (uint8_t)s_mi.cfg_len;
    s_hdr[1] = s_hdr[2] = s_hdr[3] = 0;
    memcpy(s_hdr + 4, s_mi.cfg, s_mi.cfg_len);
    s_hdr_len = 4u + s_mi.cfg_len;
    s_left = 0;
    s_data_off = 0;
    s_fpos = 0xFFFFFFFFu;
    s_mp4 = 1;
    t->bytes = s_hdr_len + 4u * s_mi.nsamples + s_mi.data_bytes;
    s_stream_left = t->bytes;
    s_kbps = 0;
    return 0;
}

/* WavPack: core B gets the whole file. Rate, channels, sample size and length come
 * from the first block header ("wvpk"); custom rates, DSD and v3 files are refused. */
static int open_wv(fs_file_t *f, as_track_t *t)
{
    static const uint32_t k_rates[15] = { 6000, 8000, 9600, 11025, 12000, 16000, 22050, 24000,
                                          32000, 44100, 48000, 64000, 88200, 96000, 192000 };
    uint8_t b[32];
    uint32_t fl, ri, total;

    if (fs_read(f, b, 32) != 32 || memcmp(b, "wvpk", 4) || le16(b + 8) < 0x402)
        return -1;
    fl = le32(b + 24);
    ri = fl >> 23 & 15u;
    if (ri == 15u || (fl & 0x80000000u))         /* custom rate in metadata; DSD */
        return -1;
    total = le32(b + 12);
    t->codec = AS_CODEC_WAVPACK;
    t->rate = k_rates[ri];
    t->channels = (fl & 4u) ? 1u : 2u;
    t->bits = (fl & 0x80u) ? 32u : ((fl & 3u) + 1u) * 8u;
    t->frames = total == 0xFFFFFFFFu || b[11] ? 0u : total;
    s_hdr_len = 0;
    s_data_off = 0;
    s_left = (uint32_t)fs_file_size(f);
    t->bytes = s_left;
    return fs_seek(f, 0) < 0 ? -1 : 0;
}

/* DSD: DSF (blocks of each channel, usually LSB first) or DFF/DSDIFF (bytes
 * interleaved, MSB first; DST-compressed refused). Core B gets a small layout
 * header, then the sound data, and makes 176.4 kHz PCM of it. */
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static int dsd_track(as_track_t *t, uint32_t layout, uint32_t ch, uint32_t dsd_rate, uint32_t block,
                     uint64_t samples, uint32_t off, uint32_t len)
{
    uint32_t mult = dsd_rate / 2822400u, h[5] = { 0x31445344u, layout, ch, mult, block };

    if (dsd_rate % 2822400u || (mult != 1 && mult != 2 && mult != 4) || ch < 1 || ch > 2 || !len)
        return -1;
    t->codec = AS_CODEC_DSD;
    t->rate = mult == 4u ? 88200u : 176400u;     /* DSD256 at 176.4k is beyond core B */
    t->channels = ch;
    t->bits = 24;
    t->frames = (uint32_t)(samples / (mult == 4u ? 128u : 16u * mult));
    memcpy(s_hdr, h, 20);
    s_hdr_len = 20;
    s_data_off = off;
    s_left = len;
    t->bytes = 20 + len;
    return 0;
}

static int open_dsf(fs_file_t *f, as_track_t *t)
{
    uint8_t b[80];
    uint64_t dsd_sz, fmt_sz, data_sz;

    if (fs_read(f, b, 28) != 28 || memcmp(b, "DSD ", 4) || (dsd_sz = le64(b + 4)) > 64u ||
        fs_seek(f, (long)dsd_sz) < 0 || fs_read(f, b, 52) != 52 || memcmp(b, "fmt ", 4) || le32(b + 16))
        return -1;                               /* format id 0 = raw DSD */
    fmt_sz = le64(b + 4);
    {
        uint32_t ch = le32(b + 24), rate = le32(b + 28), bits = le32(b + 32), block = le32(b + 44);
        uint64_t samples = le64(b + 36);
        uint32_t off = (uint32_t)(dsd_sz + fmt_sz) + 12u;

        if (fs_seek(f, (long)(dsd_sz + fmt_sz)) < 0 || fs_read(f, b, 12) != 12 || memcmp(b, "data", 4))
            return -1;
        data_sz = le64(b + 4) - 12u;
        if (bits != 1 && bits != 8)
            return -1;
        if (dsd_track(t, bits == 1 ? 0u : 1u, ch, rate, block, samples, off, (uint32_t)data_sz))
            return -1;
        return fs_seek(f, (long)off) < 0 ? -1 : 0;
    }
}

static int open_dff(fs_file_t *f, as_track_t *t)
{
    uint8_t b[16];
    uint32_t pos = 16, ch = 0, rate = 0, size = (uint32_t)fs_file_size(f);

    if (fs_read(f, b, 16) != 16 || memcmp(b, "FRM8", 4) || memcmp(b + 12, "DSD ", 4))
        return -1;
    while (pos + 12u <= size) {
        uint64_t n;

        if (fs_seek(f, (long)pos) < 0 || fs_read(f, b, 12) != 12)
            return -1;
        n = be64(b + 4);
        if (!memcmp(b, "PROP", 4)) {
            uint32_t q = pos + 16;               /* after "SND " */

            while (q + 12u <= pos + 12u + n) {
                uint64_t m;

                if (fs_seek(f, (long)q) < 0 || fs_read(f, b, 16) != 16)
                    return -1;
                m = be64(b + 4);
                if (!memcmp(b, "FS  ", 4))
                    rate = be32(b + 12);
                else if (!memcmp(b, "CHNL", 4))
                    ch = (uint32_t)b[12] << 8 | b[13];
                else if (!memcmp(b, "CMPR", 4) && memcmp(b + 12, "DSD ", 4))
                    return -1;                   /* DST */
                q += 12u + (uint32_t)m + (uint32_t)(m & 1u);
            }
        } else if (!memcmp(b, "DSD ", 4)) {
            if (!ch || dsd_track(t, 2, ch, rate, 0, n / ch * 8u, pos + 12u, (uint32_t)n))
                return -1;
            return fs_seek(f, (long)(pos + 12u)) < 0 ? -1 : 0;
        }
        pos += 12u + (uint32_t)n + (uint32_t)(n & 1u);
    }
    return -1;
}

/* Ogg (Vorbis, Opus): core B gets the whole file and its decoder trims by granule
 * positions; here: the codec, rate, channels from the first page's packet, and the
 * length from the last page's granule position (Opus: less the pre-skip). */
static int open_ogg(fs_file_t *f, as_track_t *t)
{
    uint8_t b[64], tail[512];
    uint32_t size = (uint32_t)fs_file_size(f), last = 0, from;
    long n;

    if (mp3_rd(f, 0, b, sizeof b) != sizeof b || memcmp(b, "OggS", 4) || b[26] < 1)
        return -1;
    {
        const uint8_t *p = b + 27 + b[26];          /* the first packet */

        if (p + 19 > b + sizeof b)
            return -1;
        if (p[0] == 1 && !memcmp(p + 1, "vorbis", 6)) {
            t->codec = AS_CODEC_VORBIS;
            t->channels = p[11];
            t->rate = le32(p + 12);
        } else if (!memcmp(p, "OpusHead", 8)) {
            t->codec = AS_CODEC_OPUS;
            t->channels = p[9] ? 2u : 0u;          /* opusfile mixes any layout to stereo */
            t->rate = 48000;
            t->trim = le16(p + 10);                  /* pre-skip (opusfile applies it) */
        } else {
            return -1;
        }
    }
    if (t->channels < 1 || t->channels > 2 || !t->rate)
        return -1;
    /* last page: scan back from the end for its capture pattern */
    for (from = size > sizeof tail ? size - sizeof tail : 0; ; from = from > 448u ? from - 448u : 0) {
        n = (long)mp3_rd(f, from, tail, sizeof tail);
        for (long i = n - 14; i >= 0; i--)
            if (!memcmp(tail + i, "OggS", 4) && (tail[i + 5] & 4u)) {   /* end of stream */
                last = le32(tail + i + 6);
                break;
            }
        if (last || from == 0 || size - from > 0x10000u)
            break;
    }
    t->frames = t->codec == AS_CODEC_OPUS ? (last > t->trim ? last - t->trim : 0) : last;
    t->trim = 0;                                     /* the decoder does it */
    t->bits = 16;
    s_hdr_len = 0;
    s_data_off = 0;
    s_left = size;
    t->bytes = size;
    return fs_seek(f, 0) < 0 ? -1 : 0;
}

/* Monkey's Audio: a 24-byte header (ape_info.h), then only the frames: the tags at
 * both ends, the seek table and the stored WAV header never leave the eMMC.
 * Core-B instructions per stereo frame by compression level (fast .. insane), as
 * measured in QEMU (24-bit, tests/bench.py APE*; insane estimated from its filter
 * taps). A stream that would need more than B_BUDGET instructions a second is
 * skipped: it could only play with gaps (insane at any rate, extra high above 96 kHz). */
static const uint16_t k_ape_cost[5] = { 420, 650, 950, 2470, 10200 };
#define B_BUDGET 240000000u

static int open_ape(fs_file_t *f, as_track_t *t)
{
    ape_info_t ai;
    uint32_t secs;

    if (ape_info(mp3_rd, f, (uint32_t)fs_file_size(f), &ai) != 0 || fs_seek(f, (long)ai.data_off) < 0)
        return -1;
    if ((uint64_t)k_ape_cost[ai.level / 1000u - 1u] * ai.rate * (ai.channels + 1u) > 3ull * B_BUDGET) {   /* mono: 2/3 */
        log_printf("audio: APE level %u at %u Hz is too slow for core B", (unsigned)ai.level, (unsigned)ai.rate);
        return -1;
    }
    t->codec = AS_CODEC_APE;
    t->rate = ai.rate;
    t->channels = ai.channels;
    t->bits = ai.bits;
    t->frames = ai.samples;
    ape_stream_header(&ai, s_hdr);
    s_hdr_len = APE_STREAM_HDR;
    s_data_off = ai.data_off;
    s_left = ai.data_end - ai.data_off;
    t->bytes = s_hdr_len + s_left;
    secs = ai.samples / ai.rate;
    s_kbps = secs ? s_left / 125u / secs : 0u;
    return 0;
}

/* WMA (ASF): core B gets "u32 length + asf_cfg_t" (the stream's WAVEFORMATEX and the
 * packet size), then the data packets as stored -- all of one size, so the count fixes
 * the length and whole sectors go straight to the ring; core B takes the payloads
 * apart (asf.c). WMA has no gapless trim: every decoded frame plays (t->frames = 0);
 * the play duration less the preroll is only shown. */
static uint32_t s_show_frames;             /* frames to show when t->frames is 0 */

static int open_wma(fs_file_t *f, as_track_t *t)
{
    asf_info_t ai;

    _Static_assert(4u + sizeof(asf_cfg_t) <= sizeof s_hdr, "s_hdr holds the ASF config");
    if (asf_info(mp3_rd, f, (uint32_t)fs_file_size(f), &ai) != 0 || fs_seek(f, (long)ai.data_off) < 0)
        return -1;
    if (ai.cfg.codec_id == ASF_WMAV1 || ai.cfg.codec_id == ASF_WMAV2)
        t->codec = AS_CODEC_WMA;
    else if (ai.cfg.codec_id == ASF_WMAPRO)
        t->codec = AS_CODEC_WMAPRO;
    else
        t->codec = AS_CODEC_WMALOSSLESS;
    if (ai.cfg.channels < 1 || ai.cfg.channels > 2)
        return -1;
    t->rate = ai.cfg.rate;
    t->channels = ai.cfg.channels;
    t->bits = t->codec != AS_CODEC_WMA && ai.cfg.bits > 16 ? 24 : 16;     /* Pro, Lossless: 24-bit sources */
    t->frames = 0;
    s_show_frames = ai.frames;
    s_hdr[0] = (uint8_t)sizeof ai.cfg;
    s_hdr[1] = s_hdr[2] = s_hdr[3] = 0;
    memcpy(s_hdr + 4, &ai.cfg, sizeof ai.cfg);
    s_hdr_len = 4u + sizeof ai.cfg;
    s_data_off = ai.data_off;
    s_left = ai.data_bytes;
    t->bytes = s_hdr_len + s_left;
    return 0;
}

static void close_track(void)
{
    if (s_file)
        fs_close(s_file);
    if (s_tab)
        fs_close(s_tab);
    s_file = s_tab = NULL;
    s_mp4 = 0;
}

/* Open the next track the play queue hands out and queue it for B; a file that
 * does not open is skipped. */
static int open_next(void)
{
    char name[FS_NAME_MAX];

    for (int tries = 0; tries < Q_MAX; tries++) {
        as_track_t t;
        fs_ref_t ref;
        int kind, pos;

        if (queue_take(&ref, &kind, &pos) != 0)
            return -1;
        if (fs_ref_name(&ref, name, sizeof name) != 0)
            strcpy(name, "?");
        s_file = fs_open_ref(&ref);
        if (!s_file) {
            log_printf("audio: cannot open %s", name);
            continue;
        }
        memset(&t, 0, sizeof t);
        s_kbps = 0;
        s_mp4 = 0;
        s_show_frames = 0;
        if ((kind == DL_FLAC ? open_flac(s_file, &t) :
             kind == DL_WAV ? open_wav(s_file, &t) :
             kind == DL_AIFF ? open_aiff(s_file, &t) :
             kind == DL_MP3 ? open_mp3(s_file, &t) :
             kind == DL_OGG || kind == DL_OPUS ? open_ogg(s_file, &t) :
             kind == DL_WV ? open_wv(s_file, &t) :
             kind == DL_DSF ? open_dsf(s_file, &t) :
             kind == DL_DFF ? open_dff(s_file, &t) :
             kind == DL_APE ? open_ape(s_file, &t) :
             kind == DL_WMA ? open_wma(s_file, &t) :
             kind == DL_M4A ? open_m4a(s_file, &ref, &t) : -1) != 0) {
            log_printf("audio: %s: not a playable stream (size %ld)", name, fs_file_size(s_file));
            close_track();
            continue;
        }
        if (!s_mp4) {                        /* every format: the data lies inside the file */
            uint32_t size = (uint32_t)fs_file_size(s_file);

            if (s_left && s_data_off >= size) {
                log_printf("audio: %s: data at %u, past the end (%u B)", name, (unsigned)s_data_off,
                           (unsigned)size);
                close_track();
                continue;
            }
            if (s_left > size - s_data_off) {    /* cut short: play what is there */
                s_left = size - s_data_off;
                t.bytes = s_hdr_len + s_left;
                if (t.codec != AS_CODEC_WAVPACK && t.codec != AS_CODEC_VORBIS && t.codec != AS_CODEC_OPUS)
                    t.frames = 0;            /* the count no longer holds: play what decodes */
            }
        }
        t.seq = ++s_seq;
        /* file byte p lands at ring offset in_wr + skip + hdr + (p - start): make that
         * congruent to p mod 4, so whole sectors can be read straight into the ring */
        t.skip = s_mp4 ? 0u : (s_data_off - (AS->in_wr + s_hdr_len)) & 3u;
        t.bytes += t.skip;
        s_skip_left = t.skip;
        s_hdr_pos = 0;
        {
            audio_track_t *u = &s_tracks[t.seq % AS_QUEUE];

            u->seq = t.seq;
            u->rate = t.rate;
            u->bits = t.bits;
            u->channels = t.channels;
            u->frames = t.frames ? t.frames : s_show_frames;
            u->codec = t.codec;
            u->kbps = s_kbps;
            u->index = (uint32_t)pos;
            u->ref = ref;
            strncpy(u->name, name, sizeof u->name - 1);
            u->name[sizeof u->name - 1] = 0;
        }
        AS->q[AS->q_wr % AS_QUEUE] = t;
        __asm__ volatile ("dmb" ::: "memory");   /* the entry before the count that publishes it */
        AS->q_wr++;
        log_printf("audio: queued %u '%s' codec %u %u Hz %u bit %u ch, %u frames, %u bytes",
                   (unsigned)t.seq, name, (unsigned)t.codec, (unsigned)t.rate, (unsigned)t.bits,
                   (unsigned)t.channels, (unsigned)t.frames, (unsigned)t.bytes);
        if (!s_rate)
            s_rate = t.rate;
        return 0;
    }
    return -1;
}

/* Top up the input ring: header bytes, then file data, then the next file. */
static void refill(void)
{
    uint8_t *ring = (uint8_t *)AS_IN_RING;
    uint32_t start = AS->in_wr;

    /* up to 32 KiB per tick (bounds the time spent here), in as many pieces as it
     * takes: M4A comes as a 4-byte length then a few hundred bytes, per packet */
    for (int guard = 0; guard < 512 && AS->in_wr - start < 0x8000u; guard++) {
        uint32_t used = AS->in_wr - AS->in_rd, free = AS_IN_SIZE - used;
        uint32_t off = AS->in_wr % AS_IN_SIZE, k = AS_IN_SIZE - off;

        if (!s_file) {
            if (AS->in_closed || AS->q_wr - AS->q_rd >= AS_QUEUE - 1u)
                return;
            if (open_next() != 0) {
                log_printf("audio: end of the playlist after %u tracks", (unsigned)s_seq);
                AS->in_closed = 1;           /* end of the playlist */
                return;
            }
            continue;
        }
        if (AS->abandon_seq == s_seq) {      /* B gave up on this track: stop reading it */
            log_printf("audio: track %u abandoned by core B (damaged?), %u bytes not read",
                       (unsigned)s_seq, (unsigned)(s_mp4 ? s_stream_left : s_left + s_skip_left));
            close_track();
            s_left = s_stream_left = s_skip_left = 0;
            s_hdr_len = s_hdr_pos = 0;
            AS->cut_end = AS->in_wr;
            __asm__ volatile ("dmb" ::: "memory");
            AS->cut_seq = s_seq;
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
            if (s_mp4)
                s_stream_left -= n;
            continue;
        }
        if (s_mp4 && !s_left) {             /* next sample: its length, then its bytes */
            uint32_t at, len;
            int r = mp4_next(mp3_rd, s_tab, &s_mi, &s_cur, &at, &len);

            if (r == 1 && 4u + len <= s_stream_left) {
                s_hdr[0] = (uint8_t)len;
                s_hdr[1] = (uint8_t)(len >> 8);
                s_hdr[2] = (uint8_t)(len >> 16);
                s_hdr[3] = (uint8_t)(len >> 24);
                s_hdr_len = 4;
                s_hdr_pos = 0;
                s_left = len;
                if (at != s_fpos && fs_seek(s_file, (long)at) < 0)
                    r = -1;
                s_fpos = at;
                if (r == 1)
                    continue;
            }
            if (s_stream_left) {            /* B was promised these bytes: zeros (it skips them) */
                uint32_t n = s_stream_left < k ? s_stream_left : k;

                if (r != 0)
                    log_printf("audio: sample table error %d at sample %u", r, (unsigned)s_cur.sample);
                memset(ring + off, 0, n);
                s_stream_left -= n;
                AS->in_wr += n;
                s_left = 0;
                s_hdr_len = s_hdr_pos = 0;
                continue;
            }
            close_track();
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
                static uint32_t logged;     /* (once per track: a bad card would flood the log) */

                if (logged != s_seq)
                    log_printf("audio: read error %ld, %u bytes left of track %u", n, (unsigned)s_left,
                               (unsigned)s_seq);
                logged = s_seq;
                memset(ring + off, 0, k);   /* so the rest of the track reads as zeros */
                n = (long)k;
            }
            {
                s_left -= (uint32_t)n;
                AS->in_wr += (uint32_t)n;
                if (s_mp4) {
                    s_stream_left -= (uint32_t)n;
                    s_fpos += (uint32_t)n;
                }
            }
            if (s_left || s_mp4)
                continue;
        }
        close_track();
    }
}

/* ---- core B ------------------------------------------------------------- */

/* Core-B images: one per codec family (Makefile COREB_FAMILIES). */
static const char *family_of(uint32_t codec)
{
    return codec == AS_CODEC_AAC ? "baac" : codec == AS_CODEC_VORBIS ? "bogg" :
           codec == AS_CODEC_OPUS ? "bopus" : codec == AS_CODEC_WAVPACK ? "bwv" :
           codec == AS_CODEC_DSD ? "bdsd" : codec == AS_CODEC_APE ? "bape" : codec == AS_CODEC_WMA ? "bwma" :
           codec == AS_CODEC_WMAPRO ? "bwmapro" : codec == AS_CODEC_WMALOSSLESS ? "bwmall" : "bmain";
}

static const char *s_b_image;            /* module now in core B, NULL: none */

/* Load the image into B's code RAM and let B boot; B keeps the stream state
 * in the shared block, so a swap mid-stream continues where it was. */
#define B_BOOT_MS 1000u                      /* core B: release to ready (load not included) */

static int b_load(const char *image)
{
    uint32_t t0, t_load;
    int n;

    clock_set_core_b_div(1);                 /* start at full speed; B scales down from its load */
    t0 = time_ms();
    if (s_b_up) {                            /* park B first: no exception may be half-handled */
        AS->cmd = AS_CMD_PARK;
        while (AS->state != AS_B_PARKED && time_ms() - t0 < 100u)
            cpu_wait_tick();
    }
    REG(CRU_SOFTRST3) = (CAL_CORE_RST << 16) | CAL_CORE_RST;     /* hold B in reset */
    s_b_up = 0;
    s_b_image = NULL;
    AS->cmd = AS_CMD_NONE;
    n = module_load(image, (void *)AS_B_RAM, AS_B_IMAGE_MAX);
    if (n < 0) {
        log_printf("player: core-B image %s: %d", image, n);
        return -1;
    }
    t_load = time_ms() - t0;
    t0 = time_ms();                          /* B's own boot budget starts now: the first play of a
                                              * boot used to spend the 300 ms on the cold file read
                                              * (device 2026-10-16: "cannot play (-1)" once, then fine) */
    AS->run = 0;                             /* B idles after boot until A restarts it */
    AS->state = AS_B_BOOT;
    AS->b_hz = clock_core_b_hz();
    AS->b_div = AS->b_div_req = clock_pll_hz() / clock_core_b_hz();
    REG(CRU_SOFTRST3) = CAL_CORE_RST << 16;                      /* release */
    while (AS->state != AS_B_READY) {
        if (time_ms() - t0 > B_BOOT_MS) {
            log_printf("player: core B not ready %u ms after its release (state %u, image %s %d B "
                       "loaded in %u ms)", (unsigned)(time_ms() - t0), (unsigned)AS->state, image, n,
                       (unsigned)t_load);
            return -2;
        }
        cpu_wait_tick();
    }
    log_printf("player: B up with %s (%d B): loaded in %u ms, booted in %u ms", image, n,
               (unsigned)t_load, (unsigned)(time_ms() - t0));
    s_b_image = image;
    s_b_up = 1;
    return 0;
}

/* ---- DMA + I2S ------------------------------------------------------------ */

static uint32_t s_blk_words = LLI_CTL_H;  /* block size of the chain dma_start() runs */
static void halt(void);

static void dma_prepare(void)
{
    volatile uint32_t *lli = (volatile uint32_t *)AS_LLI;

    s_blk_words = LLI_CTL_H;
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
    DMA_CHR(0x1C) = s_blk_words;
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

static void out_start(void)
{
    if (s_usb) {
        usb_out_stream_start();
        s_dma_on = 1;
    } else {
        dma_start();
    }
}

static void out_stop(void)
{
    if (s_usb) {
        usb_out_stream_stop();
        s_dma_on = 0;
    } else {
        dma_stop();
    }
}

/* The chain still plays (the DMA channel, or USB, has not reached its end). */
static int out_running(void)
{
    return s_usb ? usb_out_streaming() : (DMA_CHEN & (1u << DMA_CH)) != 0;
}

/* ---- volume: CS43131 PCM volume A/B (MAP 0x090001/0x090002), 0.5 dB steps,
 * both chips; -40 dB at power-on like RECHO (the chip's own default is 0 dB,
 * full scale into the headphones). ------------------------------------------ */

#define VOL_MIN_DB (-60)
static int s_vol_db = -40;
static int s_ext;                    /* an external source plays (the USB DAC): it owns the DAC volume */

static void dac_volume(void)
{
    uint8_t v = (uint8_t)(-s_vol_db * 2);

    for (int chan = 0; chan < 2; chan++) {
        dac_write_reg(chan, 0x090001u, v);
        dac_write_reg(chan, 0x090002u, v);
    }
}

/* ---- the output: the jack (I2S DMA, DAC) or a USB audio device (usb_out.h, which takes
 * the PCM ring's LLI chain as the DMA would) -------------------------------------------- */

static int out_rate(uint32_t rate, uint32_t bits)
{
    if (s_usb)
        return usb_out_set_format(rate, bits) == 0 ? 0 : -5;
    if (!s_dac_up) {
        int r = dac_init(rate);

        bootcrumb(BC_LOOP);                  /* (dac_trace() moved the trail into dac_init) */
        if (r != 0) {
            log_printf("audio: DAC init %d at step %d", r, dac_last_step());
            return -4;
        }
        s_dac_up = 1;
    } else if (dac_set_rate(rate) != 0) {
        return -4;
    }
    return 0;
}

static void out_volume(void)
{
    if (s_usb)
        usb_out_set_volume(s_vol_db);
    else if (s_dac_up && !s_ext)
        dac_volume();
}

int audio_set_volume_db(int db)
{
    s_vol_db = db < VOL_MIN_DB ? VOL_MIN_DB : db > 0 ? 0 : db;
    out_volume();
    return s_vol_db;
}

int audio_get_volume_db(void)
{
    return s_vol_db;
}

/* ---- external PCM source (the USB DAC) -------------------------------------
 * The DMA plays a short ring that someone else fills (I2S words, L then R):
 * AUDIO_EXT_BLOCKS blocks of AUDIO_EXT_FRAMES frames at AS_PCM_RING. The player
 * stops and core B is held in reset: nothing decodes. */

static void ext_prepare(void)
{
    volatile uint32_t *lli = (volatile uint32_t *)AS_LLI;

    s_blk_words = AUDIO_EXT_FRAMES * 2u;
    for (uint32_t i = 0; i < AUDIO_EXT_BLOCKS; i++) {
        volatile uint32_t *l = lli + i * 7u;

        l[0] = AS_PCM_RING + i * AUDIO_EXT_FRAMES * 8u;
        l[1] = I2S_TXDR;
        l[2] = AS_LLI + ((i + 1u) % AUDIO_EXT_BLOCKS) * 28u;
        l[3] = LLI_CTL_L;
        l[4] = s_blk_words;
        l[5] = l[6] = 0;
    }
    memset((void *)AS_PCM_RING, 0, AUDIO_EXT_BLOCKS * AUDIO_EXT_FRAMES * 8u);
}

int32_t *audio_ext_ring(void)
{
    return (int32_t *)AS_PCM_RING;
}

int audio_ext_start(uint32_t rate)
{
    if (s_playing || s_b_up)
        halt();
    if (s_b_up) {                            /* core B: parked, then held in reset */
        uint32_t t0 = time_ms();

        AS->cmd = AS_CMD_PARK;
        while (AS->state != AS_B_PARKED && time_ms() - t0 < 100u)
            cpu_wait_tick();
        REG(CRU_SOFTRST3) = (CAL_CORE_RST << 16) | CAL_CORE_RST;
        s_b_up = 0;
        s_b_image = NULL;
    }
    if (!s_dac_up) {
        int r = dac_init(rate);

        bootcrumb(BC_LOOP);                  /* (dac_trace() moved the trail into dac_init) */
        if (r != 0) {
            log_printf("audio: DAC init %d at step %d", r, dac_last_step());
            return -4;
        }
        s_dac_up = 1;
    } else if (dac_set_rate(rate) != 0) {
        return -4;
    }
    dac_volume();
    s_rate = rate;
    ext_prepare();
    dma_start();
    s_ext = 1;
    log_printf("audio: external source at %u Hz", (unsigned)rate);
    return 0;
}

int audio_ext_rate(uint32_t rate)
{
    if (!s_ext)
        return -1;
    if (rate == s_rate)
        return 0;
    dma_stop();
    if (dac_set_rate(rate) != 0)
        return -4;
    log_printf("audio: external source %u -> %u Hz", (unsigned)s_rate, (unsigned)rate);
    s_rate = rate;
    ext_prepare();
    dma_start();
    return 0;
}

uint32_t audio_ext_pos(void)
{
    uint32_t off = DMA_CHR(0x00) - AS_PCM_RING;

    return off / 8u % (AUDIO_EXT_BLOCKS * AUDIO_EXT_FRAMES);
}

void audio_ext_stop(void)
{
    if (!s_ext)
        return;
    dma_stop();
    s_ext = 0;
    if (s_dac_up)
        dac_volume();                        /* the player's volume alone again */
    log_printf("audio: external source off");
}

/* ---- API ---------------------------------------------------------------- */

static void halt(void)
{
    if (s_dma_on)
        out_stop();
    if (s_usb)
        usb_out_release();                   /* the device idles until the next play */
    close_track();
    if (s_b_up) {
        AS->run = 0;
        AS->cmd = AS_CMD_STOP;
        for (uint32_t t0 = time_ms(); AS->cmd && time_ms() - t0 < 200u; )
            cpu_wait_tick();
    }
    s_playing = 0;
    s_paused = 0;
}

int audio_play(int pos)
{
    static int s_as_init;
    const char *image;

    halt();
    if (!s_as_init) {
        memset((void *)AS_SHARED, 0, sizeof(audio_shared_t));
        AS->magic = AS_MAGIC;
        AS->version = AS_VERSION;
        s_as_init = 1;
    }
    queue_seek(pos);
    s_rate = 0;
    AS->in_wr = AS->in_rd = 0;
    AS->q_wr = AS->q_rd = 0;
    AS->in_closed = 0;
    AS->pcm_wr = AS->pcm_played = AS->pcm_end = 0;
    AS->underruns = AS->err = 0;
    AS->abandon_seq = AS->cut_seq = 0;
    AS->fault_pc = AS->fault_lr = 0;
    s_sv_aborts = AS->aborts;
    s_underruns = 0;
    s_sv_ticks_at = s_sv_pcm_at = time_ms();
    AS->cur_seq = AS->done_seq = 0;
    dma_prepare();
    refill();
    if (AS->q_wr == 0)
        return -3;                           /* nothing playable */
    image = family_of(AS->q[0].codec);
    if (!s_b_up || s_b_image != image) {
        int r = b_load(image);

        if (r != 0) {
            log_printf("audio: play: core B start failed (%d)", r);
            return -1;
        }
    } else if (AS->b_div != 1u) {            /* same image: back to full speed for the start */
        clock_set_core_b_div(1);
        AS->b_hz = clock_core_b_hz();
        AS->b_div = AS->b_div_req = 1;
    }
    {
        int r = out_rate(s_rate, AS->q[0].bits);

        if (r != 0) {
            log_printf("audio: play: output at %u Hz %u-bit failed (%d)", (unsigned)s_rate,
                       (unsigned)AS->q[0].bits, r);
            return r;
        }
    }
    out_volume();                            /* before the first sample reaches the DAC */
    AS->rate = s_rate;
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
    int pos;

    audio_status(&st);
    if (!st.seq)
        return -3;
    if (dir > 0)
        pos = queue_next_pos();
    else if (st.frames <= 3u * st.track.rate)       /* near the start: the previous track */
        pos = queue_prev_pos();
    else
        pos = queue_pos();                           /* restart this one */
    return pos < 0 ? -3 : audio_play(pos);
}

/* Pause: stop the I2S transfer; the DMA then stalls on its handshake and keeps
 * its place, core B stops once the ring is full. Resume restarts I2S. */

int audio_pause(int on)
{
    if (!s_playing || !s_dma_on || on == s_paused)
        return s_paused;
    if (s_usb)
        usb_out_pause(on);
    else if (on)
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

static void stream_switch(void);

/* B fell behind the DMA (input late, or its clock too low): back to full speed,
 * B scales down again from its load. Logged at most once a second. */
static void check_underrun(void)
{
    uint32_t n = AS->underruns;

    if (n == s_underruns)
        return;
    if (AS->b_div != 1u) {
        clock_set_core_b_div(1);
        AS->b_hz = clock_core_b_hz();
        AS->b_div = AS->b_div_req = 1;
    }
    if (!s_underrun_log || time_ms() - s_underrun_log >= 1000u) {
        log_printf("audio: underrun (%u in all), input %u B buffered", (unsigned)n,
                   (unsigned)(AS->in_wr - AS->in_rd));
        s_underrun_log = time_ms() | 1u;
    }
    s_underruns = n;
}

/* ---- the core-B supervisor ----------------------------------------------- */

/* Core B faulted, locked up or stopped making progress on a track (a decoder gone
 * wrong on damaged data): reload its image and go on with the track after the one
 * it was decoding. Gives up after as many failures in a row as the queue is long. */
static void b_recover(const char *why)
{
    uint32_t seq = AS->cur_seq;
    const audio_track_t *t = &s_tracks[seq % AS_QUEUE];
    int pos = seq && t->seq == seq ? (int)t->index + 1 : queue_pos() + 1;

    log_printf("audio: core B %s on track %u '%s' (pc %08x lr %08x): reloading it", why, (unsigned)seq,
               seq && t->seq == seq ? t->name : "?", (unsigned)AS->fault_pc, (unsigned)AS->fault_lr);
    if (pos >= queue_count())
        pos = queue_mode() == Q_REPEAT_ALL ? 0 : -1;
    s_b_up = 0;                              /* audio_play() loads a fresh image */
    if (pos < 0 || ++s_sv_fails > (uint32_t)queue_count() || audio_play(pos) != 0) {
        log_printf("audio: stopped after core B failed");
        halt();
    }
}

static void supervise(void)
{
    uint32_t now = time_ms();

    if (!s_b_up || s_paused)
        return;
    if (AS->aborts != s_sv_aborts) {         /* a decoder gave up on a track (it ended there) */
        s_sv_aborts = AS->aborts;
        log_printf("audio: decoder gave up on track %u (code %x)", (unsigned)AS->cur_seq,
                   (unsigned)AS->abort_code);
    }
    if (AS->done_seq != s_sv_done) {         /* a track decoded to its end: B is fine */
        s_sv_done = AS->done_seq;
        s_sv_fails = 0;
    }
    if (AS->state == AS_B_FAULT) {
        b_recover("faulted");
        return;
    }
    if (AS->ticks != s_sv_ticks) {
        s_sv_ticks = AS->ticks;
        s_sv_ticks_at = now;
    } else if (now - s_sv_ticks_at > 1000u) {
        b_recover("stopped (no heartbeat)");
        return;
    }
    /* progress = frames decoded (pcm_wr also moves on underruns, while B is stuck) */
    if (AS->cur_frames + AS->cur_seq != s_sv_pcm || AS->state != AS_B_PLAYING ||
        AS->pcm_wr - AS->pcm_played >= AS_PCM_BLOCKS ||
        (AS->in_wr - AS->in_rd < AS->in_need && !AS->in_closed)) {   /* (waiting for input: a slow card) */
        s_sv_pcm = AS->cur_frames + AS->cur_seq;   /* progressing, or waiting for good reason */
        s_sv_pcm_at = now;
    } else if (now - s_sv_pcm_at > 3000u) {
        b_recover("made no progress for 3 s");
    }
}

void audio_poll(void)
{
    if (!s_playing)
        return;
    supervise();
    if (!s_playing)
        return;
    check_underrun();
    if (AS->b_div_req != AS->b_div) {        /* core B asked for another clock */
        clock_set_core_b_div(AS->b_div_req);
        AS->b_hz = clock_core_b_hz();
        AS->b_div = AS->b_div_req;
    }
    refill();
    /* start output once B has filled the ring (or the whole stream is shorter) */
    if (!s_dma_on && !AS->cmd &&
        (AS->pcm_wr >= AS_PCM_BLOCKS || (AS->pcm_end && AS->pcm_wr >= AS->pcm_end))) {
        out_start();
        log_printf("audio: output on, %u blocks ready, B at %u MHz", (unsigned)AS->pcm_wr,
                   (unsigned)(AS->b_hz / 1000000u));
    }
    /* B ended the DMA chain: once the last block has played and been recycled,
     * either the stream is over, or the next track needs another sample rate */
    if (s_dma_on && !out_running() && AS->pcm_played == AS->pcm_wr) {
        if (AS->state == AS_B_RATE)
            stream_switch();
        else if (AS->state == AS_B_ENDED) {
            log_printf("audio: end of stream, %u tracks, B err %x, B heap peak %u B, stack free %u B, "
                       "%u underruns", (unsigned)AS->done_seq, (unsigned)AS->err, (unsigned)AS->b_heap_peak,
                       (unsigned)AS->b_stack_free, (unsigned)AS->underruns);
            halt();
        }
    }
}

/* The next track needs another sample rate or another core-B image. The last
 * sample before it has played, so: swap the image if needed (B keeps the stream
 * state in the shared block), re-clock if needed (ACODEC PLL, I2S divider, the
 * DAC's speed byte), then B refills the ring and the DMA starts as at PLAY. */
static void stream_switch(void)
{
    const as_track_t *n = &AS->q[AS->q_rd % AS_QUEUE];
    const char *image = family_of(n->codec);
    uint32_t rate = n->rate;

    out_stop();
    if (image != s_b_image) {
        log_printf("audio: core B %s -> %s", s_b_image ? s_b_image : "-", image);
        if (b_load(image) != 0) {
            halt();
            return;
        }
    }
    if (s_b_up && AS->b_div != 1u) {         /* full speed while the ring refills */
        clock_set_core_b_div(1);
        AS->b_hz = clock_core_b_hz();
        AS->b_div = AS->b_div_req = 1;
    }
    if (rate != s_rate) {
        log_printf("audio: rate %u -> %u Hz", (unsigned)s_rate, (unsigned)rate);
        if (out_rate(rate, n->bits) != 0) {
            halt();
            return;
        }
        s_rate_changes++;
    }
    dma_prepare();
    s_rate = rate;
    AS->rate = rate;
    AS->cmd = AS_CMD_RESTART;
    AS->run = 1;
}

int audio_set_output(int usb)
{
    if (!usb == !s_usb)
        return 0;
    if (s_playing)
        halt();
    s_usb = usb;
    if (!usb)
        s_dac_up = 0;                        /* the jack: DAC (and amp) brought up again at the next play */
    log_printf("audio: output %s", usb ? "USB" : "the jack");
    return 1;
}

int audio_output_usb(void)
{
    return s_usb;
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
        queue_heard((int)t->index);
        /* decoded minus what still waits in the ring */
        {
            int32_t blocks = (int32_t)(AS->pcm_wr - AS->pcm_played);   /* two reads: may be off by one */
            uint32_t buffered = (uint32_t)(blocks < 0 ? 0 : blocks > (int32_t)AS_PCM_BLOCKS ?
                                           (int32_t)AS_PCM_BLOCKS : blocks) * AS_BLOCK_FRAMES;

            st->frames = AS->cur_frames > buffered ? AS->cur_frames - buffered : 0;
        }
    }
    st->b_load_pm = s_b_up ? AS->b_load_pm : 0;
    st->b_hz = s_b_up ? AS->b_hz : 0;
    st->underruns = s_b_up ? AS->underruns : 0;
    st->err = s_b_up ? AS->err : 0;
    return 0;
}
