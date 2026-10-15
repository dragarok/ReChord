/*
 * test_tags.c -- host tests for the tag reader (services/tags.c).
 *
 * Usage: test_tags <fixtures dir> [<GAPLESS dir>] [fuzz iterations] [seed]
 * (run_tags.sh makes the fixtures and runs this under ASan/UBSan).
 *
 * 1. Expected fields from every synthetic fixture (gen_tag_fixtures.py) and
 *    from RECHO's gapless test set, with the rd() calls and bytes each costs.
 * 2. Fuzz: the same files with random bytes changed where the reader looked
 *    (and sometimes cut short); the reader must stay inside the file and its
 *    buffers, ask for no more than its window, and return valid UTF-8.
 */
#include "tags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_checks, s_failures;

#define CHECK(cond, ...) do {                                              \
    s_checks++;                                                            \
    if (!(cond)) {                                                         \
        printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);             \
        printf(__VA_ARGS__);                                               \
        printf("\n");                                                      \
        s_failures++;                                                      \
    }                                                                      \
} while (0)

#define MAX_LOG 64

typedef struct {
    char name[64];
    uint8_t *d;
    uint32_t size;
    int kind;
    uint32_t calls, bytes, bad;
    uint32_t nlog, log_off[MAX_LOG], log_len[MAX_LOG];   /* what the clean run read */
} file_t;

static uint32_t rd(void *u, uint32_t off, void *buf, uint32_t n)
{
    file_t *f = u;

    f->calls++;
    if (n > 512u || off > f->size || n > f->size - off) {
        f->bad++;                      /* the reader asked past the end, or for more than its window */
        return 0;
    }
    memcpy(buf, f->d + off, n);
    f->bytes += n;
    if (f->nlog < MAX_LOG) {
        f->log_off[f->nlog] = off;
        f->log_len[f->nlog++] = n;
    }
    return n;
}

static int kind_of(const char *name)
{
    const char *e = strrchr(name, '.');

    return !e ? TAGS_MP3 : !strcmp(e, ".flac") ? TAGS_FLAC : !strcmp(e, ".wav") ? TAGS_WAV :
           !strcmp(e, ".m4a") ? TAGS_MP4 : !strcmp(e, ".ogg") || !strcmp(e, ".opus") ? TAGS_OGG : TAGS_MP3;
}

static int load(file_t *f, const char *dir, const char *name)
{
    char p[512];
    FILE *fp;
    long n;

    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (!(fp = fopen(p, "rb")))
        return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->d = malloc(n ? (size_t)n : 1u);  /* exactly the file: ASan sees any overrun */
    f->size = (uint32_t)n;
    f->kind = kind_of(name);
    if (fread(f->d, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

/* NUL-terminated within cap and valid UTF-8 (no overlongs, surrogates). */
static int valid(const char *s, size_t cap)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t n = strnlen(s, cap);

    if (n >= cap)
        return 0;
    for (size_t i = 0; i < n; ) {
        uint32_t c = p[i], k = c < 0x80 ? 1 : c >= 0xC2 && c < 0xE0 ? 2 : c >= 0xE0 && c < 0xF0 ? 3 : c >= 0xF0 && c < 0xF5 ? 4 : 0, v;

        if (!k || i + k > n)
            return 0;
        v = k == 1 ? c : c & (0x7Fu >> k);
        for (uint32_t j = 1; j < k; j++) {
            if ((p[i + j] & 0xC0) != 0x80)
                return 0;
            v = v << 6 | (p[i + j] & 0x3F);
        }
        if ((k == 3 && v < 0x800) || (k == 4 && (v < 0x10000 || v > 0x10FFFF)) || (v >= 0xD800 && v < 0xE000))
            return 0;
        i += k;
    }
    return 1;
}

static int run(file_t *f, uint32_t size, tag_info_t *t)
{
    f->calls = f->bytes = f->bad = f->nlog = 0;
    return tags_read(rd, f, size, f->kind, t);
}

/* ---- expected fields ---------------------------------------------------- */

#define ID3V2 TAGS_SRC_ID3V2
#define APE   TAGS_SRC_APE
#define END   TAGS_SRC_ID3V2_END
#define V1    TAGS_SRC_ID3V1
#define VORB  TAGS_SRC_VORBIS
#define RIFF  TAGS_SRC_RIFF
#define MP4   TAGS_SRC_MP4
#define OGG   TAGS_SRC_OGG

typedef struct {
    int gapless;                       /* 0: fixtures dir, 1: GAPLESS dir */
    const char *name, *title, *artist, *album;
    int track, found;
} case_t;

#define S31(x) x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x
#define S15(x) x x x x x x x x x x x x x x x

static const case_t k_cases[] = {
    /* RECHO's gapless set: -META has the tags, the rest has none */
    { 1, "MP3-44-V2-META/01.mp3", "MP3-44-V2-META 01", "EchoNano", "Gapless, stuffed", 1, ID3V2 | V1 },
    { 1, "MP3-44-V2-META/02.mp3", "MP3-44-V2-META 02", "EchoNano", "Gapless, stuffed", 2, ID3V2 | V1 },
    { 1, "MP3-44-V2-META/03.mp3", "MP3-44-V2-META 03", "", "", 0, ID3V2 },
    { 1, "MP3-44-V2-META/04.mp3", "MP3-44-V2-META 04", "", "", 0, ID3V2 },
    { 1, "MP3-44-V2/01.mp3", "", "", "", 0, 0 },
    { 1, "MP3-44-V2/02.mp3", "", "", "", 0, 0 },
    { 1, "MP3-44-V2/03.mp3", "", "", "", 0, 0 },
    { 1, "MP3-44-V2/04.mp3", "", "", "", 0, 0 },
    { 1, "FLAC-44-16/01.flac", "", "", "", 0, 0 },
    { 1, "FLAC-44-16/02.flac", "", "", "", 0, 0 },
    { 1, "FLAC-44-16/03.flac", "", "", "", 0, 0 },
    { 1, "FLAC-44-16/04.flac", "", "", "", 0, 0 },
    { 1, "FLAC-44-16/05.flac", "", "", "", 0, 0 },
    { 1, "FLAC-96-24/01.flac", "", "", "", 0, 0 },
    { 1, "FLAC-96-24/02.flac", "", "", "", 0, 0 },
    { 1, "FLAC-96-24/03.flac", "", "", "", 0, 0 },
    { 1, "WAV-44-16/01.wav", "", "", "", 0, 0 },
    { 1, "WAV-44-16/02.wav", "", "", "", 0, 0 },
    { 1, "WAV-44-16/03.wav", "", "", "", 0, 0 },

    /* synthetic (gen_tag_fixtures.py) */
    { 0, "v22.mp3", "Café del Mar", "Ólafur Arnalds", "Album 22", 7, ID3V2 },
    { 0, "v23_utf16.mp3", "東京事変 – 群青日和 𝄞", "椎名林檎", "教育", 3, ID3V2 },
    { 0, "v24_utf8.mp3", "Ünïcødé Title", "Sigur Rós", "Ágætis byrjun", 12, ID3V2 },
    { 0, "v24_unsync.mp3", "ÿÿ Unsync ÿ", "Artist ÿ", "Plain", 0, ID3V2 },
    { 0, "v24_tag_unsync.mp3", "ÿ Tag ÿ", "", "", 0, ID3V2 },
    { 0, "v23_tag_unsync.mp3", "From v1", "V1 Artist", "V1 Album", 9, V1 },
    { 0, "v1_only.mp3", "Beyoncé Song", "Beyoncé", "Café Album", 4, V1 },
    { 0, "ape_hdr.mp3", "日本語のタイトル", "Ape Artist", "Ape Album", 2, APE },
    { 0, "ape_nohdr_lyrics_v1.mp3", "Ape Title", "V1 Artist", "V1 Album", 5, APE | V1 },
    { 0, "appended.mp3", "Appended Title", "Appended Artist", "V1 Album", 6, END | V1 },
    { 0, "precedence.mp3", "Front Title", "Ape Artist", "App Album", 8, ID3V2 | APE | END | V1 },
    { 0, "two_front.mp3", "First", "Second Artist", "", 0, ID3V2 },
    { 0, "trunc.mp3", S31("漢"), S31("é"), "a" S15("🎵"), 42, ID3V2 },
    { 0, "ffmpeg_v23.mp3", "Ffmpeg 東京 v3", "Björk", "Homogenic", 5, ID3V2 },
    { 0, "ffmpeg_v24.mp3", "Ffmpeg 東京 v4", "Björk", "Homogenic", 5, ID3V2 },
    { 0, "info.wav", "Motörhead Live", "張學友", "Odd", 11, RIFF },
    { 0, "id3chunk.wav", "WAV ID3 Title", "WAV ID3 Artist", "", 3, ID3V2 },
    { 0, "info_then_id3.wav", "Info Title", "", "Id3 Album", 0, RIFF | ID3V2 },
    { 0, "ffmpeg.wav", "Ffmpeg Wav", "Ffmpeg Artist", "Ffmpeg Album", 9, RIFF },
    { 0, "vorbis_pic.flac", "Flac Títle", "First Artist", "Flac Album", 4, VORB },
    { 0, "id3_flac.flac", "Id3 On Flac", "First Artist", "Flac Album", 4, ID3V2 | VORB },
    { 0, "bad_id3_size.mp3", "Big", "", "", 0, ID3V2 },
    { 0, "bad_frame_size.mp3", "", "", "", 0, 0 },
    { 0, "bad_ape.mp3", "", "", "", 0, 0 },
    { 0, "bad_vorbis.flac", "", "", "", 0, 0 },
    { 0, "bad_riff.wav", "", "", "", 0, 0 },
    { 0, "aac_end.m4a", "M4A Títle 東京", "M4A Artist", "M4A Album", 3, MP4 },
    { 0, "alac_fast.m4a", "Alac Title", "Alac Artist", "", 7, MP4 },
    { 0, "vorbis.ogg", "Ogg Títle", "Ogg Artist", "Ogg Album", 5, OGG },
    { 0, "opus_long.opus", "Opus Títle", "Opus Artist", "", 2, OGG },
    { 0, "ogg_none.ogg", "", "", "", 0, 0 },
    { 0, "bad_mp4.m4a", "", "", "", 0, 0 },
    { 0, "bad_ogg.ogg", "", "", "", 0, 0 },
    { 0, "tiny.mp3", "", "", "", 0, 0 },
    { 0, "empty.mp3", "", "", "", 0, 0 },
};
#define NCASES (sizeof k_cases / sizeof k_cases[0])

static file_t s_files[NCASES];
static int s_loaded[NCASES];

/* ---- fuzz --------------------------------------------------------------- */

static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void fuzz(long iters)
{
    uint32_t idx[NCASES], n = 0, bad = 0, invalid = 0, found = 0;
    unsigned long calls = 0;

    for (uint32_t i = 0; i < NCASES; i++)
        if (s_loaded[i] && s_files[i].size)
            idx[n++] = i;
    if (!n)
        return;
    for (long it = 0; it < iters; it++) {
        file_t *f = &s_files[idx[rnd() % n]];
        uint32_t nm = 1 + rnd() % 6, off[6], len[6], size = f->size, nlog = f->nlog;
        static uint8_t save[6][512];
        tag_info_t t;

        for (uint32_t m = 0; m < nm; m++) {
            uint32_t r = rnd() % 10, v = rnd();

            if (r < 8 && nlog) {       /* where the reader looked: sizes, ids, flags, text */
                uint32_t l = rnd() % nlog;

                off[m] = f->log_off[l] + rnd() % (f->log_len[l] < 64 ? f->log_len[l] : 64);
            } else {
                off[m] = rnd() % f->size;
            }
            r = rnd() % 8;             /* a byte, a 32-bit size, or a run without NULs */
            len[m] = r < 4 ? 1 : r < 6 ? 4 : 1 + rnd() % 512;
            if (len[m] > f->size - off[m])
                len[m] = f->size - off[m];
            memcpy(save[m], f->d + off[m], len[m]);
            for (uint32_t i = 0; i < len[m]; i++) {
                uint8_t *p = f->d + off[m] + i;

                *p = r == 0 ? 0xFF : r == 1 ? 0x00 : r == 2 ? (uint8_t)(*p ^ (1u << (v % 8))) :
                     r == 3 ? (uint8_t)v : r == 4 ? (uint8_t)(v >> (8 * i)) :
                     r == 5 ? (i ? 0xFF : 0x7F) : r == 6 ? (uint8_t)('A' + i % 26) : (uint8_t)(0x80 | rnd());
            }
        }
        if (rnd() % 8 == 0)
            size = rnd() % (f->size + 1);
        run(f, size, &t);
        bad += f->bad;
        calls += f->calls;
        found += t.found != 0;
        invalid += !valid(t.title, sizeof t.title) || !valid(t.artist, sizeof t.artist) || !valid(t.album, sizeof t.album);
        for (uint32_t m = nm; m--; )
            memcpy(f->d + off[m], save[m], len[m]);
        /* the log is now the mutated run's, so mutations wander down new
         * paths; a clean run now and then brings them back */
        if (it % 64 == 0)
            run(f, f->size, &t);
    }
    CHECK(bad == 0, "%u out-of-range or oversized rd() requests", bad);
    CHECK(invalid == 0, "%u results that are not NUL-terminated UTF-8", invalid);
    printf("fuzz: %ld iterations over %u files, %lu rd() calls, %u with a field\n", iters, n, calls, found);
}

int main(int argc, char **argv)
{
    const char *fix = argc > 1 ? argv[1] : ".work/tags";
    const char *gap = argc > 2 ? argv[2] : "";
    long iters = argc > 3 ? atol(argv[3]) : 5000;
    uint32_t total_calls = 0;

    if (argc > 4)
        s_rng = (uint32_t)strtoul(argv[4], 0, 0) | 1u;
    printf("%-26s %5s %6s  %s\n", "file", "calls", "bytes", "found");
    for (uint32_t i = 0; i < NCASES; i++) {
        const case_t *k = &k_cases[i];
        file_t *f = &s_files[i];
        tag_info_t t;
        int ret;

        if (load(f, k->gapless ? gap : fix, k->name)) {
            if (k->gapless) {
                printf("%-26s SKIP (no GAPLESS dir)\n", k->name);
                continue;
            }
            CHECK(0, "cannot open %s/%s (run gen_tag_fixtures.py)", fix, k->name);
            continue;
        }
        s_loaded[i] = 1;
        ret = run(f, f->size, &t);
        printf("%-26s %5u %6u  0x%02x  %s | %s | %s | %u\n", k->name, f->calls, f->bytes, t.found,
               t.title, t.artist, t.album, t.track);
        total_calls += f->calls;
        CHECK(ret == (k->found ? 0 : -1), "%s: returned %d", k->name, ret);
        CHECK(!strcmp(t.title, k->title), "%s: title \"%s\"", k->name, t.title);
        CHECK(!strcmp(t.artist, k->artist), "%s: artist \"%s\"", k->name, t.artist);
        CHECK(!strcmp(t.album, k->album), "%s: album \"%s\"", k->name, t.album);
        CHECK(t.track == k->track, "%s: track %u", k->name, t.track);
        CHECK(t.found == k->found, "%s: found 0x%02x", k->name, t.found);
        CHECK(f->bad == 0, "%s: %u bad rd() requests", k->name, f->bad);
        CHECK(f->calls <= 8, "%s: %u rd() calls", k->name, f->calls);   /* covers are seeked over */
        CHECK(valid(t.title, sizeof t.title) && valid(t.artist, sizeof t.artist) && valid(t.album, sizeof t.album),
              "%s: not UTF-8", k->name);
    }
    printf("rd() calls in all: %u\n", total_calls);
    fuzz(iters);
    printf("test_tags: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
