/*
 * dirlist.h -- one folder, sorted the way people sort: folders first, then files,
 * case ignored, numbers by value ("2" before "10"). Only folders and playable
 * files are kept; macOS "._" files and dot entries are hidden. A folder with more
 * than DL_MAX entries is shown a page of DL_MAX (on-disk order) at a time, each
 * page sorted.
 */
#ifndef RECHORD_DIRLIST_H
#define RECHORD_DIRLIST_H

#include <stdint.h>

#include "fs.h"

#define DL_MAX 400

/* Playable file kinds (by extension), 0 = not playable. */
enum { DL_NONE = 0, DL_FLAC, DL_WAV, DL_AIFF, DL_MP3, DL_M4A, DL_OGG, DL_OPUS, DL_WV, DL_DSF, DL_DFF,
       DL_APE, DL_WMA, DL_KINDS };
int dirlist_kind(const char *name);

/* Load page `page` of the folder at path (or of a folder ref when path is NULL).
 * Returns the entry count (0..DL_MAX), or FS_ERR_*. *more = entries beyond this page. */
int dirlist_load(const char *path, const fs_ref_t *dir, int page, int *more);
int dirlist_count(void);
const char *dirlist_name(int i);
const fs_ref_t *dirlist_ref(int i);
int dirlist_is_dir(int i);
int dirlist_kind_at(int i);

/* Natural order (folders not considered): < 0, 0, > 0. */
int dirlist_cmp(const char *a, const char *b);

#endif
