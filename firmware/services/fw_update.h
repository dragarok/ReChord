/*
 * fw_update.h -- install the board's update image (8 GB: HIFIECnn.IMG / ECHOMINI.IMG,
 * 512 MB: MINIVnnn.IMG / MINIMINI.IMG) from the user volume
 * (FAT32) into both eMMC firmware slots (reference updater 0x03054474).
 */
#ifndef RECHORD_FW_UPDATE_H
#define RECHORD_FW_UPDATE_H

#include <stdint.h>

#define FWU_NONE 1           /* no update image on the volume */

/* Negative results: */
#define FWU_EIO      (-1)    /* eMMC read failed */
#define FWU_EFS      (-2)    /* volume is not FAT32 */
#define FWU_EFRAG    (-3)    /* image in more than FWU_EXTENTS pieces (copy it again) */
#define FWU_EFORMAT  (-4)    /* header / size / header copy mismatch */
#define FWU_ECRC     (-5)    /* trailer CRC mismatch */
#define FWU_EFIT     (-6)    /* larger than one firmware slot */
#define FWU_EVERIFY  (-7)    /* read-back after write differs */
#define FWU_EWRITE   (-8)    /* eMMC write failed */

/* The pieces of the file on the volume, in file order: a host (macOS, Windows) fills free holes,
 * so a large file is often fragmented (the user's own volume: 24 pieces for stock's 32 MiB). */
#define FWU_EXTENTS 128

typedef struct {
    char name[13];           /* "MINIV900.IMG" */
    uint8_t raw[11];         /* 8.3 directory name */
    uint32_t lba;            /* first sector of the file */
    uint32_t n_ext;          /* pieces in ext[]; 0 = one, contiguous from lba (tests) */
    struct { uint32_t lba, n; } ext[FWU_EXTENTS];   /* sectors */
    uint32_t size;           /* bytes */
    uint32_t fw_end;         /* bytes installed into each slot */
    uint32_t dir_lba, dir_off;
    uint8_t sha256[32];      /* whole file, as `sha256sum` prints it */
} fw_image_t;

/* progress(what, done, total) may be NULL. */
typedef void (*fwu_progress_t)(const char *what, uint32_t done, uint32_t total);

/* Finds and fully validates the image. 0 = valid, FWU_NONE, or a negative
 * FWU_E* (img->name is set whenever a file was found). */
int fw_update_find(uint32_t vol_lba, fw_image_t *img, fwu_progress_t progress);

/* Writes slot 2, then copies it to slot 1; each slot's header block goes
 * last and every block is read back. 0 or a negative FWU_E*. */
/* Both firmware slots valid (as the boot checks them)? If exactly one is, it is copied over the
 * other (the order of fw_update_install). 0 both fine, 1 slot 1 repaired, 2 slot 2 repaired,
 * < 0 an error (FWU_EIO: neither is valid). */
int fw_update_repair(fwu_progress_t progress);
int fw_update_install(const fw_image_t *img, fwu_progress_t progress);

/* Deletes the image (as stock does) so it isn't offered again. */
int fw_update_retire(const fw_image_t *img);

#endif
