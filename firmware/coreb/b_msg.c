/*
 * b_msg.c -- core B's lines for core A's log (audio_shared.h b_msg): in every image (the EQ's lines
 * come from all of them). Moved here unchanged from b_hifi.c.
 */
#include <stdarg.h>
#include <stdint.h>

#include "b_rt.h"

static char *put_u(char *d, char *e, uint32_t v, unsigned base, int width)
{
    char t[12];
    int n = 0;

    do {
        t[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n < width && n < 12)
        t[n++] = '0';
    while (n && d < e)
        *d++ = t[--n];
    return d;
}

void b_msg(const char *fmt, ...)
{
    uint32_t w = AS->b_msg_wr;
    char *d, *e;
    int width;
    va_list ap;

    if (w - AS->b_msg_rd >= 4u)
        return;                              /* core A has not caught up: dropped */
    d = AS->b_msg[w % 4u];
    e = d + sizeof AS->b_msg[0] - 1;
    va_start(ap, fmt);
    for (; *fmt && d < e; fmt++) {
        if (*fmt != '%' || !fmt[1]) {
            *d++ = *fmt;
            continue;
        }
        width = 0;
        if (fmt[1] == '0' && fmt[2] >= '1' && fmt[2] <= '9') {   /* %0Nu, %0Nx */
            width = fmt[2] - '0';
            fmt += 2;
        }
        switch (*++fmt) {
        case 'u': d = put_u(d, e, va_arg(ap, unsigned), 10, width); break;
        case 'x': d = put_u(d, e, va_arg(ap, unsigned), 16, width); break;
        case 'd': {
            int v = va_arg(ap, int);

            if (v < 0 && d < e)
                *d++ = '-';
            d = put_u(d, e, v < 0 ? 0u - (unsigned)v : (unsigned)v, 10, 0);
            break;
        }
        case 's':
            for (const char *s = va_arg(ap, const char *); *s && d < e; )
                *d++ = *s++;
            break;
        default:
            *d++ = *fmt;
        }
    }
    va_end(ap);
    *d = 0;
    __asm__ volatile ("dmb" ::: "memory");
    AS->b_msg_wr = w + 1u;
}

