/*
 * player_audio.c -- the player.h API on top of the dual-core audio engine
 * (firmware/audio). Replaces player.c, whose decoders ran on core A and fed an
 * IPC ring nothing consumed. Opening a file plays its directory from that file
 * on, gapless across tracks.
 */
#include "services/bootcrumb.h"
#include "services/log.h"
#include "drivers/dac_cs43131.h"
#include <stdlib.h>
#include <string.h>

#include "player.h"
#include "audio/audio.h"
#include "services/fs.h"
#include "services/dirlist.h"
#include "services/queue.h"
#include "services/log.h"
#include "services/config.h"
#include "services/tags.h"

static char s_name[FS_NAME_MAX];
static tag_info_t s_tags;                    /* of the heard track, when cfg tags=on */
static uint32_t s_tags_seq;
static int s_open;
static char s_src[150];                      /* where the queue came from (for resume) */
static int s_tree;

/* "f:<first cluster>:<folder>" / "t:...": the queue's source and the heard track. */
void player_note_track(uint32_t clus)
{
    if (!s_src[0])
        return;
    snprintf(g_cfg.track, sizeof g_cfg.track, "%c:%lu:%s", s_tree ? 't' : 'f', (unsigned long)clus, s_src);
}

/* Kept for browser_screen's error text (the core-A FLAC adapter is gone). */
unsigned flac_diag_block, flac_diag_channels;
unsigned long flac_diag_need;
int flac_diag_reason;

int player_init(void) { return PLAYER_OK; }

/* Queue the file's folder (sorted), starting at the file. */
int player_open(const char *path)
{
    const char *slash = strrchr(path, '/');
    char dir[160];
    size_t dl = slash ? (size_t)(slash - path) : 0;
    int n, incomplete;

    if (!slash || dl >= sizeof dir || strlen(slash + 1) >= sizeof s_name)
        return PLAYER_ERR;
    memcpy(dir, path, dl);
    dir[dl] = 0;
    strcpy(s_name, slash + 1);
    s_open = 0;
    for (int page = 0, more = 1; more; page++) {     /* find the file's ref */
        if ((n = dirlist_load(dl ? dir : "/", NULL, page, &more)) < 0)
            return PLAYER_ERR;
        for (int i = 0; i < n; i++)
            if (!dirlist_is_dir(i) && !strcmp(dirlist_name(i), s_name)) {
                fs_ref_t start = *dirlist_ref(i);

                if (queue_from_folder(dir, NULL, &start, &incomplete) != 0 || !queue_count())
                    return PLAYER_ERR;
                snprintf(s_src, sizeof s_src, "%s", dir);
                s_tree = 0;
                s_open = 1;
                return PLAYER_OK;
            }
    }
    return PLAYER_ERR;
}

/* Everything under a folder, subfolders included (RECHO: hold play on a folder). */
int player_open_tree(const char *path)
{
    int incomplete;

    s_open = 0;
    if (queue_from_tree(path, NULL, &incomplete) != 0 || !queue_count())
        return PLAYER_ERR_EMPTY;
    if (incomplete)
        log_printf("player: '%s' queued in part: %d tracks", path, queue_count());
    snprintf(s_src, sizeof s_src, "%s", path);
    s_tree = 1;
    s_open = 1;
    return PLAYER_OK;
}

int player_play(void)
{
    if (audio_paused()) {
        audio_pause(0);
        return PLAYER_OK;
    }
    if (!s_open)
        return PLAYER_ERR_EMPTY;
    {   /* the result in the boot trail (raw sectors, carried to the next boot): no device log has
         * ever held a playback session (2026-10-16), the trail survives what ended those boots */
        static uint32_t s_plays;
        int r = audio_play(queue_pos());

        s_plays++;
        bootcrumb_set(BCK_PLAY, ((uint32_t)r & 0xFFFFu) | s_plays << 16);
        if (r != 0) {
            log_printf("player: play failed: %d (DAC step %d)", r, dac_last_step());
            bootcrumb_set(BCK_PLAY_FAIL, ((uint32_t)r & 0xFFFFu) | (uint32_t)dac_last_step() << 16);
        }
        return r == 0 ? PLAYER_OK : PLAYER_ERR;
    }
}

int player_mode(void) { return queue_mode(); }

/* Next play mode; the tracks already handed to core B still play first. */
int player_cycle_mode(void)
{
    queue_set_mode((queue_mode() + 1) % Q_MODES);
    g_cfg.play = (uint8_t)queue_mode();
    config_save_later();
    log_printf("ui: play mode %s", queue_mode_name(queue_mode()));
    return queue_mode();
}

int player_pause(void)
{
    audio_pause(1);
    return audio_paused() ? PLAYER_OK : PLAYER_ERR_STATE;
}

int player_stop(void)
{
    audio_stop();
    return PLAYER_OK;
}

int player_seek(long ms) { return PLAYER_ERR_STATE; }
int player_next(void) { return audio_skip(1) == 0 ? PLAYER_OK : PLAYER_ERR_EMPTY; }
int player_prev(void) { return audio_skip(-1) == 0 ? PLAYER_OK : PLAYER_ERR_EMPTY; }

int player_get_state(void)
{
    audio_status_t st;

    audio_status(&st);
    return st.err ? PLAYER_ERROR : audio_paused() ? PLAYER_PAUSED : st.playing ? PLAYER_PLAYING
                                                                              : PLAYER_STOPPED;
}

long player_get_position_ms(void)
{
    audio_status_t st;

    audio_status(&st);
    return st.track.rate ? (long)((uint64_t)st.frames * 1000u / st.track.rate) : 0;
}

int player_set_volume(int db)
{
    g_cfg.volume = (int16_t)audio_set_volume_db(db);
    config_save_later();
    return PLAYER_OK;
}

/* Resume "track": the saved queue, from the saved track. 0, or < 0. */
int player_resume(const char *track)
{
    unsigned long clus;
    char kind = track[0], *p;
    int incomplete;

    if ((kind != 'f' && kind != 't') || track[1] != ':')
        return -1;
    clus = strtoul(track + 2, &p, 10);
    if (*p++ != ':')
        return -1;
    if (kind == 't' ? queue_from_tree(p, NULL, &incomplete) : queue_from_folder(p, NULL, NULL, &incomplete))
        return -2;
    for (int i = 0; i < queue_count(); i++)
        if (queue_ref(i)->clus == clus) {
            queue_seek(i);
            break;
        }
    snprintf(s_src, sizeof s_src, "%s", p);
    s_tree = kind == 't';
    s_open = 1;
    return 0;
}
int player_get_volume(void) { return audio_get_volume_db(); }

static uint32_t tag_read_at(void *u, uint32_t off, void *buf, uint32_t n)
{
    long got;

    if (fs_seek((fs_file_t *)u, (long)off) < 0)
        return 0;
    got = fs_read((fs_file_t *)u, buf, n);
    return got > 0 ? (uint32_t)got : 0u;
}

/* The heard track's tags, read once per track (a few small reads: covers are
 * seeked over). Kinds tags.c knows: ID3v2/ID3v1/APEv2 (MP3, APE, WavPack, ...),
 * FLAC Vorbis comments, WAV chunks, MP4 ilst (M4A, ALAC), Ogg Vorbis/Opus. */
static void load_tags(const audio_track_t *t)
{
    int kind = dirlist_kind(t->name);
    fs_file_t *f;

    memset(&s_tags, 0, sizeof s_tags);
    if (kind == DL_WMA || kind == DL_DSF || kind == DL_DFF || !(f = fs_open_ref(&t->ref)))
        return;
    tags_read(tag_read_at, f, (uint32_t)fs_file_size(f),
              kind == DL_FLAC ? TAGS_FLAC : kind == DL_WAV ? TAGS_WAV : kind == DL_M4A ? TAGS_MP4 :
              kind == DL_OGG || kind == DL_OPUS ? TAGS_OGG : TAGS_MP3, &s_tags);
    fs_close(f);
}

int player_get_track(track_info_t *out)
{
    audio_status_t st;

    memset(out, 0, sizeof *out);
    audio_status(&st);
    if (st.seq && g_cfg.tags && st.seq != s_tags_seq) {
        s_tags_seq = st.seq;
        load_tags(&st.track);
    }
    if (st.seq && g_cfg.tags && s_tags.title[0]) {
        strncpy(out->title, s_tags.title, sizeof out->title - 1);
        strncpy(out->artist, s_tags.artist, sizeof out->artist - 1);
        out->track_no = s_tags.track;
    } else {
        strncpy(out->title, st.seq ? st.track.name : s_name, sizeof out->title - 1);
    }
    if (st.track.rate)
        out->duration_ms = (long)((uint64_t)st.track.frames * 1000u / st.track.rate);
    out->total_samples = (long)st.track.frames;
    return PLAYER_OK;
}

int player_register_codec(const char *ext, const codec_t *codec) { return PLAYER_OK; }
int player_enqueue(const char *path) { return PLAYER_ERR; }

int player_process(void)
{
    audio_poll();
    return PLAYER_OK;
}

void player_set_callback(player_event_fn fn, void *user) { }
void player_set_replaygain_mode(int mode) { }
int player_get_replaygain_mode(void) { return 0; }
int player_get_replaygain_gain_tenth_db(void) { return 0; }
