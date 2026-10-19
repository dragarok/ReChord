/*
 * fs_target.h -- volumes behind the target fs backend (fs_target.c).
 * Paths are "/<volume>/dir/file"; "/" lists the mounted volumes.
 */
#ifndef RECHORD_SERVICES_FS_TARGET_H
#define RECHORD_SERVICES_FS_TARGET_H

#include <stdint.h>

#define FS_VOLUMES 2

/* Block reader: absolute device sectors, 4-byte aligned buffer, 0 = ok. FS_BLK_GONE: the medium is
 * gone (drivers/emmc.h SD_ERR_NOCARD): the call that saw it answers FS_ERR_GONE. */
typedef int (*fs_block_read_t)(uint32_t lba, uint32_t count, void *buf);
#define FS_BLK_GONE (-40)

/* Block writer, same conventions. */
typedef int (*fs_block_write_t)(uint32_t lba, uint32_t count, const void *buf);

/* Mounts FAT16 / FAT32 / exFAT found at `base` (superfloppy or MBR
 * partition 0) as volume `idx` named `name`. 0, or FS_ERR_* (unsupported
 * file system, I/O). Mounting again (e.g. after a USB session) drops every
 * cached sector of the volume; write handles on it go dead (fs_close only
 * releases them). */
int fs_target_mount(int idx, const char *name, fs_block_read_t rd, uint32_t base);

/* Same, with a block writer: a FAT32, FAT16 or exFAT volume then takes
 * fs_open_write / fs_open_append / fs_write / fs_remove / fs_mkdir /
 * fs_rename. wr = NULL, FAT12 and exFAT with two FATs (TexFAT) stay
 * read-only (FS_ERR_UNSUPPORTED / NULL). */
int fs_target_mount_rw(int idx, const char *name, fs_block_read_t rd,
                       fs_block_write_t wr, uint32_t base);
/* Writes back anything cached for the volume, then forgets it. Handles still
 * open on it are dead (FS_ERR_GONE) until closed. */
void fs_target_unmount(int idx);

/* Everything cached for the volume on the disk; exFAT: VolumeDirty cleared
 * when no write handle is open. 0 or FS_ERR_*. */
int fs_target_sync(int idx);

/* The volume serial (FAT32 boot sector 0x43, FAT16 0x27, exFAT 0x64), 0 when
 * not mounted. */
uint32_t fs_target_serial(int idx);

/* "FAT32" / "exFAT" / "FAT16", or NULL when not mounted. */
const char *fs_target_type(int idx);

/* Sectors read and written through this layer so far (both volumes; the library logs its share). */
extern uint32_t fs_target_nrd, fs_target_nwr;

/* Nonzero when volume `idx` takes writes. */
int fs_target_writable(int idx);

/* Time stamps for written files: now() returns FAT date << 16 | FAT time
 * ((year-1980) << 9 | month << 5 | day; hour << 11 | min << 5 | sec / 2).
 * NULL (default): 2026-01-01 12:00:00. */
void fs_target_set_clock(uint32_t (*now)(void));

#endif
