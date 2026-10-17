/*
 * ap_tick.c -- core A's 10 ms work, run from SysTick (drivers/clock_rknanoc.c tick_hook) at its low
 * priority, so a main-loop pass held up by the storage, a redraw or a modal screen holds up neither:
 *   - the keys: the converter stepped and sampled into the gesture recogniser (drivers keys_tick,
 *     services/input.c), timestamped (DESIGN 3.3, 4.2: a stalled pass never loses or delays a key);
 *   - core B's clock: what B asked for applied within 10 ms (audio_tick; OURS_AUDIT 3.3).
 */
#include "drivers/panel_io.h"
#include "audio/audio.h"

void tick_hook(void)
{
    keys_tick();
    audio_tick();
}
