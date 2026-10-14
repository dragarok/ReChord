/*
 * player_audio.c -- the player.h API on top of the dual-core audio engine
 * (firmware/audio). Replaces player.c, whose decoders ran on core A and fed an
 * IPC ring nothing consumed. Opening a file plays its directory from that file
 * on, gapless across tracks.
 */
#include <string.h>

#include "player.h"
#include "audio/audio.h"
#include "services/fs.h"

static char s_dir[160], s_name[FS_NAME_MAX];
static uint32_t s_index;
static int s_open;

/* Kept for browser_screen's error text (the core-A FLAC adapter is gone). */
unsigned flac_diag_block, flac_diag_channels;
unsigned long flac_diag_need;
int flac_diag_reason;

int player_init(void) { return PLAYER_OK; }

int player_open(const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t dl = slash ? (size_t)(slash - path) : 0;
    fs_dirent_t ent;
    uint32_t got, total;

    if (!slash || dl >= sizeof s_dir || strlen(slash + 1) >= sizeof s_name)
        return PLAYER_ERR;
    memcpy(s_dir, path, dl);
    s_dir[dl] = 0;
    if (!dl)
        strcpy(s_dir, "/");
    strcpy(s_name, slash + 1);
    for (s_index = 0; ; s_index++) {        /* its position in the listing */
        if (fs_list_dir(s_dir, s_index, &ent, 1, &got, &total) != 0 || !got)
            return PLAYER_ERR;
        if (!strcmp(ent.name, s_name))
            break;
    }
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
    return audio_play_dir(s_dir, s_index) == 0 ? PLAYER_OK : PLAYER_ERR;
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

int player_set_volume(int db) { audio_set_volume_db(db); return PLAYER_OK; }
int player_get_volume(void) { return audio_get_volume_db(); }

int player_get_track(track_info_t *out)
{
    audio_status_t st;

    memset(out, 0, sizeof *out);
    audio_status(&st);
    strncpy(out->title, st.seq ? st.track.name : s_name, sizeof out->title - 1);
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
