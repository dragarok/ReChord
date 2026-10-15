/*
 * codecs.h -- ReChord stand-in for Rockbox's codec API header (lib/rbcodec/codecs/codecs.h).
 *
 * The vendored decoder sources (README.rechord) include this instead of Rockbox's: no
 * codec_api (ci->...) is used by them, only the attribute macros, the logging macros
 * and the byte-order helpers below. IRAM placement attributes are empty: core B runs
 * everything from its own SRAM.
 */
#ifndef RECHORD_WMA_CODECS_H
#define RECHORD_WMA_CODECS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ROCKBOX
#define ROCKBOX 1
#endif
#define ROCKBOX_LITTLE_ENDIAN 1

#define IBSS_ATTR
#define ICODE_ATTR
#define ICONST_ATTR
#define IDATA_ATTR
#define ICODE_ATTR_TREMOR_MDCT
#define MEM_ALIGN_ATTR __attribute__((aligned(16)))

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#define DEBUGF(...) do { } while (0)
#define LOGF(...)   do { } while (0)

#define swap16(x) __builtin_bswap16(x)
#define swap32(x) __builtin_bswap32(x)

/* big-endian 32-bit read at any address: one (unaligned) load + REV on the M3 */
static inline uint32_t rc_rb32(const void *p)
{
    uint32_t v;

    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
#define AV_RB32(x) rc_rb32(x)

#endif
