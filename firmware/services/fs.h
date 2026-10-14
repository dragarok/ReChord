/*
 * fs.h — minimal storage abstraction for the ReChord service layer.
 *
 * WHY: settings and logging must run unchanged on the host (tests) and on the
 * target (device). They talk only to this API; the backend is selected at link
 * time:
 *
 *   - fs_host.c   — POSIX backend over a real filesystem (tests, tools).
 *   - fs_target.c — FAT16/FAT32/exFAT on the device's block devices; writes
 *                   on FAT32 volumes mounted writable (fs_target.h).
 *
 * Exactly ONE backend is linked into any given image.
 *
 * Path convention: device-style volume-rooted paths, e.g. "\\RECHORD.CFG" or
 * "/MUSIC/track.flac". The host backend maps them under a configurable root
 * directory (fs_host_set_root), so tests run in a scratch dir.
 *
 * Return convention: functions that return a count/length return >= 0 on
 * success and a negative FS_ERR_* code on failure. Open calls return NULL on
 * failure.
 */
#include <stdint.h>

#ifndef RECHORD_SERVICES_FS_H
#define RECHORD_SERVICES_FS_H

#define FS_OK              0
#define FS_ERR_ARG        (-1)  /* NULL/invalid argument */
#define FS_ERR_OPEN       (-2)  /* file or directory not found / not openable */
#define FS_ERR_IO         (-3)  /* read/write failure */
#define FS_ERR_UNSUPPORTED (-4) /* operation not available in this backend */

/* Opaque file handle (backed by FILE* on the host, by an SDK file id on the
 * target). */
typedef struct fs_file fs_file_t;

/* Longest entry name we carry, including NUL (matches settings/path style). */
#ifndef FS_NAME_MAX
#define FS_NAME_MAX 256
#endif

/* Directory entry value type. Callers that collect entries (UI browser,
 * tests) use this struct; the callback API below stays pointer-light. */
typedef struct fs_dirent {
    char name[FS_NAME_MAX];
    int  is_dir;
} fs_dirent_t;

/* Directory entry callback for fs_list_dir.
 *   name   — entry name (no path prefix)
 *   is_dir — nonzero if the entry is a directory
 * Return nonzero to stop the iteration early. */
typedef int (*fs_dirent_cb)(const char *name, int is_dir);

/* List a directory. Returns the number of entries reported to cb (>= 0), or a
 * negative FS_ERR_* code. Entries "." and ".." are never reported. */
int fs_list_dir_each(const char *path, fs_dirent_cb cb);

/* Canonical directory listing for the browser: pagination without loading
 * the whole directory. Fills up to `max` entries from `index`, reports how
 * many were written (*out_count) and the total entry count (*out_total).
 * Returns 0 on success. (Canonical shape adopted at integration: the UI
 * browser needs random access pages; fs_list_dir_each stays as the simple
 * iteration form.) */
int fs_list_dir(const char *path, uint32_t index, fs_dirent_t *out,
                uint32_t max, uint32_t *out_count, uint32_t *out_total);

/* Open a file for reading. NULL on failure. */
fs_file_t *fs_open_read(const char *path);

/* Read up to <cap> bytes. Returns bytes read (0 at EOF) or FS_ERR_*. */
long fs_read(fs_file_t *f, void *buf, unsigned long cap);

/* Reposition a read handle to an absolute byte offset (seek). Returns the
 * new offset (>= 0) or a negative FS_ERR_* (ESPIPE/ARG). After seek, the
 * next fs_read resumes from `offset`; seeking past the end is legal and
 * reports FS_ERR_IO on the next read. Required by codec_io and any codec
 * with a seekable source (WAV/FLAC/MP3). */
long fs_seek(fs_file_t *f, long offset);

/* Total size of the open file in bytes, or FS_ERR_*. */
long fs_file_size(fs_file_t *f);

/* Close and release a handle opened by any fs_open_* call. For a write
 * handle this is where the data, the cluster chain and the directory entry
 * (size, first cluster, time) reach the disk: 0, or FS_ERR_IO when that
 * failed (the handle is released either way). NULL is ignored (0). */
int fs_close(fs_file_t *f);

/* Open a file for writing, creating or truncating it. NULL on failure
 * (missing directory, read-only volume, file already open, bad name). On
 * the target, new names must be 8.3 (stored upper-case); the old content
 * stays on disk until fs_close() switches to the new one. Write handles
 * are write-only: fs_read / fs_seek answer FS_ERR_UNSUPPORTED. */
fs_file_t *fs_open_write(const char *path);

/* Open a file for appending, creating it if needed. NULL on failure. */
fs_file_t *fs_open_append(const char *path);

/* Write <len> bytes at the end. Returns bytes written (short = disk full or
 * I/O error part way) or FS_ERR_*. Word-aligned buffers of whole sectors
 * go to the device without a copy. */
long fs_write(fs_file_t *f, const void *buf, unsigned long len);

/* Delete a file (not a directory, not an open file). 0 or FS_ERR_*. */
int fs_remove(const char *path);

/* 1 if the file or directory exists, else 0. */
int fs_exists(const char *path);

#endif /* RECHORD_SERVICES_FS_H */
