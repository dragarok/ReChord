/*
 * log.c -- the log (log.h): lines go into a 4 KiB byte ring in RAM; log_flush()
 * appends them to the log file in one write. When the ring is full the oldest
 * unflushed lines are kept and new ones are dropped (and counted), so a flush
 * always shows what led up to the overflow.
 */
#include "log.h"
#include "fs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RING 4096u

char log_ring[RING];                     /* not static: a debugger/QEMU can read it */
uint32_t log_ring_head, log_ring_len;    /* oldest byte, bytes held (the recent history) */
/* The last s_unwritten bytes of the history are not written to the file yet. A flush writes only those and
 * keeps the history, so the RAM ring still shows the recent lines after they reach ECHOLOG.TXT
 * (the 10 s periodic flush used to empty it, 2026-10-16); room is made by dropping the oldest
 * saved lines, and only a ring full of unsaved lines drops a new one. */
static uint32_t s_unwritten;
/* ...and the last s_unsaved bytes are not on the disk yet: written, the storage may still hold them
 * (drivers/emmc.h's queue) until log_durable(). tools/emu_run.py reads it: ECHOLOG.TXT + the
 * ring's last s_unsaved bytes is the whole log once. */
static uint32_t s_unsaved;
static unsigned long s_dropped, s_lines;
static log_clock_fn s_clock;
static int (*s_sync)(void);

void log_set_sync(int (*sync)(void))
{
    s_sync = sync;
}

void log_init(log_clock_fn clock_ms)
{
    s_clock = clock_ms;
}

unsigned long log_dropped(void)
{
    return s_dropped;
}

unsigned log_pending(void)
{
    return (unsigned)s_lines;
}

unsigned log_bytes(void)
{
    return (unsigned)s_unwritten;
}

void log_printf(const char *fmt, ...)
{
    char line[LOG_LINE_MAX];
    unsigned long ms;
    int n;
    va_list ap;

    if (!fmt)
        return;
    ms = s_clock ? s_clock() : 0ul;
    n = snprintf(line, sizeof line, "%lu.%03lu ", ms / 1000ul, ms % 1000ul);
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (unsigned)n - 1u, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';
    if (s_unwritten + (unsigned)n > RING) {
        s_dropped++;
        return;
    }
    while (log_ring_len + (unsigned)n > RING) {   /* the oldest saved line makes room */
        char c;

        do {
            c = log_ring[log_ring_head];
            log_ring_head = (log_ring_head + 1u) % RING;
            log_ring_len--;
        } while (c != '\n' && log_ring_len > s_unwritten);
    }
    for (int i = 0; i < n; i++)
        log_ring[(log_ring_head + log_ring_len + (unsigned)i) % RING] = line[i];
    log_ring_len += (unsigned)n;
    s_unwritten += (unsigned)n;
    s_unsaved += (unsigned)n;
    if (s_unsaved > log_ring_len)
        s_unsaved = log_ring_len;            /* (the oldest written lines made room) */
    s_lines++;
}

static int write_ring(void);

static int flush(const char *tag)
{
    unsigned long dropped = s_dropped;
    int r;

    if (tag)
        log_printf("log flush (%s)", tag);
    if ((r = write_ring()) != LOG_OK || !dropped)
        return r;
    log_printf("... %lu log lines dropped (buffer full)", dropped);
    return write_ring();
}

void log_durable(void)
{
    s_unsaved = s_unwritten;
}

int log_flush(const char *tag)
{
    int r = flush(tag);

    if (r == LOG_OK && tag && s_sync) {
        if (s_sync() != 0)
            r = LOG_ERR_IO;
        else
            log_durable();
    }
    if (!s_sync)
        log_durable();                       /* (written straight to the disk) */
    return r;
}

int log_flush_async(const char *tag)
{
    return flush(tag);
}

static int write_ring(void)
{
    fs_file_t *f;
    int ok = 1;

    if (!s_unwritten)
        return LOG_OK;
    f = fs_open_append(LOG_PATH);
    if (!f)
        return LOG_ERR_OPEN;
    while (ok && s_unwritten) {
        uint32_t at = (log_ring_head + log_ring_len - s_unwritten) % RING;
        uint32_t k = RING - at < s_unwritten ? RING - at : s_unwritten;

        ok = fs_write(f, log_ring + at, k) == (long)k;
        if (ok)
            s_unwritten -= k;
    }
    if (fs_close(f) != 0)
        ok = 0;
    if (!ok)
        return LOG_ERR_IO;
    s_lines = 0;
    s_dropped = 0;
    return LOG_OK;
}
