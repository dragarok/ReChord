/*
 * audio.h -- the audio engine on core A (decoding runs on core B; see audio.c).
 */
#ifndef RECHORD_AUDIO_H
#define RECHORD_AUDIO_H

#include <stdint.h>

#include "services/fs.h"

typedef struct {
    uint32_t seq, codec, rate, bits, channels, frames;
    uint32_t kbps;                 /* MP3 bit rate of the first frame, 0 otherwise */
    uint32_t index;                /* its position in the play queue (services/queue.h) */
    fs_ref_t ref;                  /* the file */
    char name[64];
} audio_track_t;

typedef struct {
    int playing;
    uint32_t seq;                  /* track being decoded, 0 = none */
    audio_track_t track;
    uint32_t frames;               /* frames of it already played (approx., block granular) */
    uint32_t b_load_pm, b_hz, underruns, err;
} audio_status_t;

/* Play the queue (services/queue.h) from play-order position pos on, gapless.
 * 0, or < 0: -1/-2 core B did not start, -3 nothing playable, -4 DAC init failed. */
int audio_play(int pos);
void audio_stop(void);
/* Call once per tick (10 ms): file -> input ring, output start/stop, B clock. */
void audio_poll(void);
int audio_status(audio_status_t *st);
/* DAC volume in dB, clamped to -60..0 (default -40); returns the value set. */
int audio_set_volume_db(int db);
int audio_get_volume_db(void);
/* +1: next track; -1: previous (or the start of this one if more than 3 s in).
 * Returns audio_play()'s result. */
int audio_skip(int dir);
/* 1 = pause, 0 = resume; returns the paused state. */
int audio_pause(int on);
int audio_paused(void);

#endif
