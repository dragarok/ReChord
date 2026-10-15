/*
 * modules.h -- blobs stored in our firmware image (tools/make_fw1.py --module):
 * the core-B decoder images. Read from the firmware slot on the eMMC.
 */
#ifndef RECHORD_MODULES_H
#define RECHORD_MODULES_H

#include <stdint.h>

/* Loads module `name` to dst (4-byte aligned, room for max bytes rounded up to a
 * sector) and checks its sum. Returns its length, or < 0: -1 no table / I/O,
 * -2 not found, -3 too big, -4 bad sum. */
int module_load(const char *name, void *dst, uint32_t max);

#endif
