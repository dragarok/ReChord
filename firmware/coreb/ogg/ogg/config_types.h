#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__
#include <stdint.h>
/* int, not int32_t: newlib's int32_t is long, and Tremor mixes int* with
 * ogg_int32_t* on the same buffers (with long, -O2's strict aliasing breaks it) */
typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int ogg_int32_t;
typedef unsigned int ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;
#endif
