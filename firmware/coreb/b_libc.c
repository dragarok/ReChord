/* core B: the few libc routines dr_flac and the ring copies need. memcpy moves
 * words when both pointers allow it (ring copies are word-aligned). */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *o = d;
    const uint8_t *i = s;

    if ((((uintptr_t)o | (uintptr_t)i) & 3u) == 0) {
        uint32_t *ow = (uint32_t *)o;
        const uint32_t *iw = (const uint32_t *)i;

        for (; n >= 16; n -= 16) {
            ow[0] = iw[0]; ow[1] = iw[1]; ow[2] = iw[2]; ow[3] = iw[3];
            ow += 4; iw += 4;
        }
        for (; n >= 4; n -= 4)
            *ow++ = *iw++;
        o = (uint8_t *)ow;
        i = (const uint8_t *)iw;
    }
    while (n--)
        *o++ = *i++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *o = d;
    const uint8_t *i = s;

    if (o <= i || o >= i + n)
        return memcpy(d, s, n);
    while (n--)
        o[n] = i[n];
    return d;
}

/* Words once aligned: FFmpeg's WMA Lossless clears up to 1 KiB of LMS history per
 * sample and filter (a byte loop made it 3x slower than real time). */
void *memset(void *d, int c, size_t n)
{
    uint8_t *o = d;

    for (; n && ((uintptr_t)o & 3u); n--)
        *o++ = (uint8_t)c;
    if (n >= 4) {
        uint32_t v = (uint8_t)c * 0x01010101u, *w = (uint32_t *)o;

        for (; n >= 16; n -= 16) {
            w[0] = v; w[1] = v; w[2] = v; w[3] = v;
            w += 4;
        }
        for (; n >= 4; n -= 4)
            *w++ = v;
        o = (uint8_t *)w;
    }
    while (n--)
        *o++ = (uint8_t)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;

    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

int abs(int v)
{
    return v < 0 ? -v : v;
}

/* Insertion sort: FAAD2 sorts SBR band tables of at most a few dozen entries. */
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    uint8_t *b = base, tmp[16];

    if (sz > sizeof tmp)
        return;
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--) {
            memcpy(tmp, b + j * sz, sz);
            memcpy(b + j * sz, b + (j - 1) * sz, sz);
            memcpy(b + (j - 1) * sz, tmp, sz);
        }
}

long labs(long v)
{
    return v < 0 ? -v : v;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;

    for (; n; n--, p++)
        if (*p == (uint8_t)c)
            return (void *)p;
    return NULL;
}

/* vorbisfile reads errno after a short read; ours never sets it */
static int s_errno;
int *__errno(void)
{
    return &s_errno;
}

char *strcpy(char *d, const char *s)
{
    char *o = d;

    while ((*o++ = *s++))
        ;
    return d;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (uint8_t)*a - (uint8_t)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

/* newlib's <ctype.h> macros index this (C locale): flags U L N S P C X B */
const char _ctype_[1 + 256] = {0,32,32,32,32,32,32,32,32,32,40,40,40,40,40,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,136,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,68,68,68,68,68,68,68,68,68,68,16,16,16,16,16,16,16,65,65,65,65,65,65,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,16,16,16,16,16,16,66,66,66,66,66,66,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,16,16,16,16,32,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
