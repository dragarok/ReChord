/*
 * lib_io.c -- the library's buffered streams (lib_int.h): a writer that hands fs_write whole,
 * word-aligned buffers (fs_target.c then writes them straight to the card, no copy), and a reader
 * with a window that keeps the current record contiguous. Merge cursors share one handle
 * (lr_attach: each refill seeks first), so a k-way merge holds one handle, not k.
 */
#include <string.h>

#include "lib_int.h"

uint8_t lib_sd_wrote;                    /* a library file on the card written since its last sync */

int lw_open(lw_t *w, const char *path, void *buf, uint32_t cap, int append)
{
    lib_sd_wrote |= !strncmp(path, "/SD/", 4);
    memset(w, 0, sizeof *w);
    w->buf = buf;
    w->cap = cap;
    w->f = append ? fs_open_append(path) : fs_open_write(path);
    if (!w->f) {
        w->err = FS_ERR_OPEN;
        return FS_ERR_OPEN;
    }
    if (append)
        w->pos = (uint32_t)fs_file_size(w->f);
    return 0;
}

extern fs_file_t *fs_open_append_at(const char *path, uint32_t at) __attribute__((weak));
extern int fs_remove_part(const char *path, uint32_t max) __attribute__((weak));

/* A step of removing a file a pass can afford (fs_remove_part: LIB_RM_CLUS clusters, at most two
 * FAT sectors at 512-byte clusters): 1 = gone (or never there), 0 = more. Each file the job is
 * about to write anew goes this way first: a rewrite frees the old chain at fs_close in one go. */
int lib_rm_step(const char *path)
{
    int r;

    if (!fs_exists(path))
        return 1;
    if (fs_remove_part && (r = fs_remove_part(path, LIB_RM_CLUS)) >= 0)
        return r;
    (void)fs_remove(path);
    return 1;
}

int lw_open_at(lw_t *w, const char *path, void *buf, uint32_t cap, uint32_t at)
{
    lib_sd_wrote |= !strncmp(path, "/SD/", 4);
    memset(w, 0, sizeof *w);
    w->buf = buf;
    w->cap = cap;
    w->f = fs_open_append_at ? fs_open_append_at(path, at) : 0;
    if (!w->f) {
        w->err = FS_ERR_OPEN;
        return FS_ERR_OPEN;
    }
    w->pos = at;
    return 0;
}

int lw_flush(lw_t *w)
{
    if (w->n && !w->err) {
        long r = fs_write(w->f, w->buf, w->n);

        if (r != (long)w->n)
            w->err = FS_ERR_IO;
    }
    w->n = 0;
    return w->err;
}

void lw_put(lw_t *w, const void *p, uint32_t n)
{
    const uint8_t *s = p;

    w->crc = lib_crc(w->crc, s, n);
    w->pos += n;
    while (n) {
        uint32_t k = w->cap - w->n;

        if (k > n)
            k = n;
        memcpy(w->buf + w->n, s, k);
        w->n += k;
        s += k;
        n -= k;
        if (w->n == w->cap)
            lw_flush(w);
    }
}

void lw_u8(lw_t *w, uint32_t v)
{
    uint8_t b = (uint8_t)v;

    lw_put(w, &b, 1);
}

void lw_u16(lw_t *w, uint32_t v)
{
    uint8_t b[2];

    lib_put16(b, v);
    lw_put(w, b, 2);
}

void lw_u32(lw_t *w, uint32_t v)
{
    uint8_t b[4];

    lib_put32(b, v);
    lw_put(w, b, 4);
}

int lw_close(lw_t *w)
{
    int r = 0;

    if (!w->f)
        return w->err ? w->err : FS_ERR_ARG;
    lw_flush(w);
    if (fs_close(w->f) != 0)
        r = FS_ERR_IO;
    w->f = 0;
    return w->err ? w->err : r;
}

void lw_abort(lw_t *w)
{
    if (w->f)
        (void)fs_close(w->f);            /* (fs_close switches what was written in: the caller's
                                          * file is incomplete, and its trailer says so) */
    w->f = 0;
    w->err = FS_ERR_IO;
}

/* ---- reader ---- */

extern void fs_place_get(fs_file_t *f, uint32_t pl[2]) __attribute__((weak));
extern void fs_place_set(fs_file_t *f, const uint32_t pl[2]) __attribute__((weak));

static void lr_init(lr_t *r, fs_file_t *f, uint32_t start, uint32_t end, void *buf, uint32_t cap)
{
    r->f = f;
    r->pl[0] = 0;
    r->buf = buf;
    r->cap = cap;
    r->n = r->i = 0;
    r->base = start;
    r->end = end;
}

int lr_open(lr_t *r, const char *path, void *buf, uint32_t cap)
{
    fs_file_t *f = fs_open_read(path);

    if (!f)
        return FS_ERR_OPEN;
    lr_init(r, f, 0, (uint32_t)fs_file_size(f), buf, cap);
    r->own = 1;
    return 0;
}

int lr_open_ref(lr_t *r, const fs_ref_t *ref, void *buf, uint32_t cap)
{
    fs_file_t *f = fs_open_ref(ref);

    if (!f)
        return FS_ERR_OPEN;
    lr_init(r, f, 0, (uint32_t)fs_file_size(f), buf, cap);
    r->own = 1;
    return 0;
}

void lr_attach(lr_t *r, fs_file_t *f, uint32_t start, uint32_t end, void *buf, uint32_t cap)
{
    lr_init(r, f, start, end, buf, cap);
    r->own = 0;
}

uint32_t lr_tell(const lr_t *r)
{
    return r->base + r->i;
}

void lr_seek(lr_t *r, uint32_t off)
{
    if (off >= r->base && off <= r->base + r->n) {
        r->i = off - r->base;
        return;
    }
    r->base = off;
    r->n = r->i = 0;
}

const uint8_t *lr_peek(lr_t *r, uint32_t n)
{
    if (n > r->cap || lr_tell(r) + n > r->end)
        return 0;
    if (r->n - r->i < n) {
        uint32_t keep = r->n - r->i, want, at;
        long got;

        memmove(r->buf, r->buf + r->i, keep);
        r->base += r->i;
        r->i = 0;
        r->n = keep;
        at = r->base + keep;
        want = r->cap - keep;
        if (want > r->end - at)
            want = r->end - at;
        if (fs_seek(r->f, (long)at) < 0)      /* (a no-op for a stream that owns its handle) */
            return 0;
        if (!r->own && fs_place_set)
            fs_place_set(r->f, r->pl);
        got = fs_read(r->f, r->buf + keep, want);
        if (got < 0)
            return 0;
        if (!r->own && fs_place_get)
            fs_place_get(r->f, r->pl);
        r->n += (uint32_t)got;
        if (r->n < n)
            return 0;
    }
    return r->buf + r->i;
}

int lr_get(lr_t *r, void *dst, uint32_t n)
{
    uint8_t *o = dst;

    while (n) {
        uint32_t k = n < r->cap ? n : r->cap;
        const uint8_t *p = lr_peek(r, k);

        if (!p)
            return FS_ERR_IO;
        memcpy(o, p, k);
        r->i += k;
        o += k;
        n -= k;
    }
    return 0;
}

const uint8_t *lr_rec(lr_t *r)
{
    const uint8_t *p = lr_peek(r, 4);
    uint32_t len;

    if (!p || (len = lib_le16(p + 2)) < 4u || !(p = lr_peek(r, len)))
        return 0;
    r->i += len;
    return p;
}

/* The next record that is not a checkpoint ('C': the walk's state, bigger than any reader's
 * buffer: seeked over, never read). */
const uint8_t *lr_rec_raw(lr_t *r)
{
    for (;;) {
        const uint8_t *p = lr_peek(r, 4);
        uint32_t len;

        if (!p || (len = lib_le16(p + 2)) < 4u)
            return 0;
        if (p[0] != LIB_RAW_C)
            return lr_rec(r);
        lr_seek(r, lr_tell(r) + len);
    }
}

const uint8_t *lr_rec16(lr_t *r)
{
    const uint8_t *p = lr_peek(r, 2);
    uint32_t len;

    if (!p || (len = lib_le16(p)) < 2u || !(p = lr_peek(r, len)))
        return 0;
    r->i += len;
    return p;
}

void lr_close(lr_t *r)
{
    if (r->own && r->f)
        fs_close(r->f);
    r->f = 0;
}
