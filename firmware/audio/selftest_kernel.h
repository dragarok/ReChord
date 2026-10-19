/*
 * selftest_kernel.h -- the clock self-test's kernels, shared by core A (services/cpuclk.c), core B
 * (coreb/b_selftest.c) and the host check (snowsky_echo_mini tests/cfw/m41_clock.sh compiles this
 * file with the host compiler and compares ST_KERNEL_SIG with what it computes).
 *
 * The 200 MHz failure on the 512 MB unit (snowsky_echo_mini docs/BRINGUP_512M.md, 2026-10-16) was a
 * datapath failure on long operations: decimal conversions came out with impossible digits while
 * the high digits were right [device; work/stock_boot/06_clock_stability.md section 0.3, inferred].
 * So the kernel leans on exactly those paths: UDIV and SDIV with large operands (a divisor that is
 * a variable, so the compiler cannot turn it into a multiply), the decimal conversion by n % 10,
 * n / 10 itself, UMULL/UMLAL, SMULL/SMLAL, MLA, CLZ, shifts and rotates by a register amount, and
 * LDM/STM (eight-word copies). Pure 32/64-bit integer arithmetic: the same result on any compiler
 * and any core, so one constant checks both cores.
 *
 * The March test is March C- [standard: van de Goor] over a window, with two backgrounds (0 and
 * 0x55555555, each with its complement), word accesses. It finds stuck-at, transition and most
 * coupling faults, and with the core at the clock under test, read/write timing faults of the
 * memory path (GRF response cycle, docs: clock_rknanoc.c mem_timing).
 */
#ifndef RECHORD_SELFTEST_KERNEL_H
#define RECHORD_SELFTEST_KERNEL_H

#include <stdint.h>

/* Results (both cores; A logs them, services/cpuclk.c; "failed at N" is the stage). */
enum {
    ST_OK = 0,
    ST_ARMED = 1,          /* persisted before the first switch of a boot (a boot that dies after it is a failure) */
    ST_PARK = 2,           /* core B did not hold for the switch (no CRU write was made) */
    ST_LOCK = 3,           /* the PLL did not lock at the point (back to the UI point) */
    ST_A_FREQ = 4,         /* core A not at its clock within 0.5 % (the 24 MHz timer) */
    ST_B_FREQ = 5,         /* core B the same */
    ST_UNLOCK = 6,         /* the PLL lock bit read 0 during the test */
    ST_A_KERNEL = 7,       /* the datapath kernel's signature on core A */
    ST_B_KERNEL = 8,
    ST_A_MARCH = 9,        /* March C- over core A's LOW window */
    ST_B_MARCH_HIGH = 10,  /* March C- over core B's free HIGH data RAM (its heap) */
    ST_B_MARCH_LOW = 11,   /* March C- over core B's LOW window (core A's SRAM, across the bus) */
    ST_A_CODE = 12,        /* core A's code checksum (against the one at the UI point) */
    ST_B_CODE = 13,        /* core B's code checksum (against core A's at its load) */
    ST_B_TIMEOUT = 14,     /* core B did not finish its part in time */
    ST_B_RESUME = 15,      /* core B did not come back after the switch (a stall) */
    ST_DIED = 16,          /* a boot ended between ST_ARMED and the result (found at the next boot) */
    ST_PERIPH = 17,        /* the boot gating (clock_periph_init) left the DMA or the VOP dead, or a boot
                            * ended inside it (services/cpuclk.c cpuclk_prepare) */
    ST_N
};

/* The LOW window: core A's SRAM 0x03048000..0x0304F000, which firmware_ap.ld leaves free ("stays
 * free for a second window"; nothing is linked or loaded there once our code runs). Core A tests
 * the first half while core B tests the second, both at once. */
#define ST_LOW_A      0x03048000u
#define ST_LOW_B      0x0304B800u
#define ST_LOW_BYTES  0x00003800u

/* The kernel's parameters and its signature (host-computed: m41 checks it). */
#define ST_KERNEL_SEED   0x2545F491u
#define ST_KERNEL_ITERS  4096u
#define ST_KERNEL_SIG    0xD2ACEC9Au   /* clang -O0/-O2 on the host; m41 recomputes it */

typedef struct { uint32_t w[8]; } st_oct_t;      /* an eight-word copy: LDM/STM */

/* One pass of the kernel. buf: 64 words of scratch (any contents: the kernel writes it first).
 * ten: 10, read through a volatile, so n % ten and n / ten are real UDIVs. */
static inline uint32_t st_kernel(uint32_t seed, uint32_t iters, uint32_t *buf, volatile const uint32_t *ten)
{
    uint32_t x = seed | 1u, acc = 0x9E3779B9u, i;
    uint64_t u64 = 0, s64 = 0;

    for (i = 0; i < 64u; i++)
        buf[i] = seed ^ (i * 0x01000193u);
    for (i = 0; i < iters; i++) {
        uint32_t d, q, n, ds, r;
        int32_t sd, sq;

        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        d = (x | 1u) >> (x & 7u);                        /* a variable divisor, never 0 */
        q = (x * 2654435761u) / d;                       /* UDIV, large operands */
        sd = (int32_t)((d & 0xFFFFu) | 1u);
        sq = (int32_t)x / sd;                            /* SDIV (sd > 0: no overflow) */
        u64 += (uint64_t)x * q;                          /* UMULL / UMLAL */
        s64 += (uint64_t)((int64_t)(int32_t)x * (int64_t)sq);   /* SMULL / SMLAL */
        acc = acc * 31u + q;                             /* MLA */
        acc ^= (uint32_t)__builtin_clz(x | 1u) << (i & 15u);    /* CLZ, shift by a register */
        r = x & 31u;
        acc = (acc << r) | (acc >> ((32u - r) & 31u));   /* rotate by a register */
        for (n = x, ds = 0; n; n /= *ten)                /* the decimal conversion */
            ds = ds * 7u + n % *ten;
        acc += ds;
        buf[i & 63u] ^= acc;
        if ((i & 63u) == 63u) {                          /* eight-word block copies */
            st_oct_t *o = (st_oct_t *)buf;
            st_oct_t t = o[(x >> 3) & 7u];

            o[(x >> 6) & 7u] = t;
        }
    }
    for (i = 0; i < 64u; i++)
        acc = (acc ^ buf[i]) * 0x01000193u;
    return acc ^ (uint32_t)u64 ^ (uint32_t)(u64 >> 32) ^ (uint32_t)s64 ^ (uint32_t)(s64 >> 32);
}

/* March C- over n words at p with background bg: 0, or the 1-based index of the first element
 * that read wrong (element k reading at word w: (k << 24) | (w & 0xFFFFFF) + 1). */
static inline uint32_t st_march(volatile uint32_t *p, uint32_t n, uint32_t bg)
{
    uint32_t i, nb = ~bg;

    for (i = 0; i < n; i++)
        p[i] = bg;
    for (i = 0; i < n; i++) {
        if (p[i] != bg)
            return (1u << 24 | i) + 1u;
        p[i] = nb;
    }
    for (i = 0; i < n; i++) {
        if (p[i] != nb)
            return (2u << 24 | i) + 1u;
        p[i] = bg;
    }
    for (i = n; i-- > 0; ) {
        if (p[i] != bg)
            return (3u << 24 | i) + 1u;
        p[i] = nb;
    }
    for (i = n; i-- > 0; ) {
        if (p[i] != nb)
            return (4u << 24 | i) + 1u;
        p[i] = bg;
    }
    for (i = n; i-- > 0; )
        if (p[i] != bg)
            return (5u << 24 | i) + 1u;
    return 0;
}

/* Both backgrounds. */
static inline uint32_t st_march2(volatile uint32_t *p, uint32_t n)
{
    uint32_t r = st_march(p, n, 0u);

    return r ? r : st_march(p, n, 0x55555555u);
}

/* A Fletcher-style checksum of n words (catches any single bit flip and any word swap). */
static inline uint32_t st_sum(const volatile uint32_t *p, uint32_t n)
{
    uint32_t s1 = 0x12345678u, s2 = 0;

    while (n--) {
        s1 += *p++;
        s2 += s1;
    }
    return s1 ^ (s2 << 1 | s2 >> 31);
}

#endif
