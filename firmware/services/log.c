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
/* The last s_unsaved bytes of the history are not on the disk yet. A flush writes only those and
 * keeps the history, so the RAM ring still shows the recent lines after they reach ECHOLOG.TXT
 * (the 10 s periodic flush used to empty it, 2026-10-16); room is made by dropping the oldest
 * saved lines, and only a ring full of unsaved lines drops a new one. */
static uint32_t s_unsaved;
static unsigned long s_dropped, s_lines;
static log_clock_fn s_clock;

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
    return (unsigned)s_unsaved;
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
    if (s_unsaved + (unsigned)n > RING) {
        s_dropped++;
        return;
    }
    while (log_ring_len + (unsigned)n > RING) {   /* the oldest saved line makes room */
        char c;

        do {
            c = log_ring[log_ring_head];
            log_ring_head = (log_ring_head + 1u) % RING;
            log_ring_len--;
        } while (c != '\n' && log_ring_len > s_unsaved);
    }
    for (int i = 0; i < n; i++)
        log_ring[(log_ring_head + log_ring_len + (unsigned)i) % RING] = line[i];
    log_ring_len += (unsigned)n;
    s_unsaved += (unsigned)n;
    s_lines++;
}

static int write_ring(void);

int log_flush(const char *tag)
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

static int write_ring(void)
{
    fs_file_t *f;
    int ok = 1;

    if (!s_unsaved)
        return LOG_OK;
    f = fs_open_append(LOG_PATH);
    if (!f)
        return LOG_ERR_OPEN;
    while (ok && s_unsaved) {
        uint32_t at = (log_ring_head + log_ring_len - s_unsaved) % RING;
        uint32_t k = RING - at < s_unsaved ? RING - at : s_unsaved;

        ok = fs_write(f, log_ring + at, k) == (long)k;
        if (ok)
            s_unsaved -= k;
    }
    if (fs_close(f) != 0)
        ok = 0;
    if (!ok)
        return LOG_ERR_IO;
    s_lines = 0;
    s_dropped = 0;
    return LOG_OK;
}
