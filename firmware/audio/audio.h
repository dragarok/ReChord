/*
 * audio.h -- the audio engine on core A (decoding runs on core B; see audio.c).
 */
#ifndef RECHORD_AUDIO_H
#define RECHORD_AUDIO_H

#include <stdint.h>

typedef struct {
    uint32_t seq, codec, rate, bits, channels, frames;
    char name[64];
} audio_track_t;

typedef struct {
    int playing;
    uint32_t seq;                  /* track being decoded, 0 = none */
    audio_track_t track;
    uint32_t frames;               /* frames of it already played (approx., block granular) */
    uint32_t b_load_pm, b_hz, underruns, err;
} audio_status_t;

/* Play the directory `dir` from entry `index` on (every FLAC/WAV after it, gapless).
 * 0, or < 0: -1/-2 core B did not start, -3 nothing playable, -4 DAC init failed. */
int audio_play_dir(const char *dir, uint32_t index);
void audio_stop(void);
/* Call once per tick (10 ms): file -> input ring, output start/stop, B clock. */
void audio_poll(void);
int audio_status(audio_status_t *st);

#endif
