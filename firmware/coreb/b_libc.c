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

void *memset(void *d, int c, size_t n)
{
    uint8_t *o = d;

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
