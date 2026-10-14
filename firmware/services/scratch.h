/*
 * scratch.h -- one 32 KiB buffer for the modes that never run at the same time:
 * USB mass storage (sector staging) and the firmware updater (block + verify).
 * Neither may hold data across the other. (They used 48 KiB as separate buffers.)
 */
#ifndef RECHORD_SCRATCH_H
#define RECHORD_SCRATCH_H

#include <stdint.h>

#define SCRATCH_WORDS 8192u
extern uint32_t g_scratch[SCRATCH_WORDS];

#endif
