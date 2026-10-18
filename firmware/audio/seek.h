/*
 * seek.h -- where to start streaming a track so that playback resumes at a given sample.
 *
 * Core A finds the coded unit (FLAC frame, WavPack block, MPEG frame) at or before the target in
 * the file, streams from there, and core B drops the decoded frames before the target (as_track_t
 * .drop): the samples after the target are the very ones a play from the start gives.
 *
 * FLAC and WavPack carry their sample position in every frame / block header, so the unit is found
 * by an interpolation search over the file (a few small reads; a bisection after six probes, so a
 * file of very uneven bit rate still converges), each candidate header checked (FLAC: CRC-8 and the
 * fields STREAMINFO fixes; WavPack: the fields, and the next block's header right after it).
 * MPEG audio frames carry no position: the frame is found from the Xing TOC (VBR) or the constant
 * frame length (CBR, or the average length when only the frame count is known), its start checked
 * by three chained headers; its index is exact for CBR, an estimate for VBR.
 *
 * Stock MINIV390's core-B codecs seek in whole seconds to a frame start (FLAC seg78 FUN_010034cc: a
 * bisection over 0xFFF8 syncs, no SEEKTABLE, within 16 frames; MP3 seg73 FUN_01003478: the average bit
 * rate, no TOC, no reservoir pre-roll): the samples after a stock seek are not the file's. RECHO 0.10
 * seeks only when it reopens a saved track (STATIC.md C.3).
 */
#ifndef RECHORD_SEEK_H
#define RECHORD_SEEK_H

#include <stdint.h>

#include "mp3_info.h"

typedef uint32_t (*seek_read_at_t)(void *u, uint32_t off, void *buf, uint32_t n);

/* FLAC: the frame holding sample t (or one before it), in the frames [lo, hi) of the file (lo =
 * the first frame, sample 0). si: the 34-byte STREAMINFO. 0 (*off: its offset, *first: its first
 * sample), or < 0 (I/O, nothing found). */
int seek_flac(seek_read_at_t rd, void *u, uint32_t lo, uint32_t hi, const uint8_t *si, uint64_t t,
              uint32_t *off, uint64_t *first);

/* WavPack: the block (its first, "initial", block for its samples) holding sample t, as above;
 * lo is the first block, total the stream's samples (0 = unknown). */
int seek_wavpack(seek_read_at_t rd, void *u, uint32_t lo, uint32_t hi, uint64_t total, uint64_t t,
                 uint32_t *off, uint64_t *first);

/* MPEG audio: the frame of index `want` (counted from mi->data_off; frames before it are the
 * pre-roll the caller chose), or the nearest valid frame start to where it should be. 0 (*off,
 * *index: the index of the frame found; exact for CBR, estimated from the TOC for VBR), or < 0. */
int seek_mp3(seek_read_at_t rd, void *u, const mp3_info_t *mi, uint32_t want, uint32_t *off, uint32_t *index);

/* FLAC's frame header CRC-8 (polynomial x^8 + x^2 + x + 1), for the host tests. */
uint8_t seek_crc8(const uint8_t *p, uint32_t n);

#endif
