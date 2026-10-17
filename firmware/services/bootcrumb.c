/*
 * bootcrumb.c -- a boot trail on the eMMC from the first line of Main (bootcrumb.h).
 *
 * The 512 MB bring-up (snowsky_echo_mini docs/BRINGUP_512M.md) found the ROM leaves the storage
 * card selected and readable (rung 1), and that polled single-sector writes through the
 * controller as it is keep working across our own clock and emmc_init (rung 2). This file does
 * the same, so the trail works before our storage driver has run, and after it:
 *   - the addressing mode is found by reading slot 1's firmware header (sector, then byte
 *     addressing) and proven by slot 2's;
 *   - the record goes to rockusb LBA 0x10EA4..0x10EA5, inside the bootlog area (slot 1's tail,
 *     past every image, all zero in the 2026-10-15 backup), and only if both sectors are zero or
 *     already ours;
 *   - sector 0: "EBLG" "RCB1", boot count, last step started, faults, values, fault pc, lr;
 *     60 key/value pairs per sector from word 8. Sector 1: "EBLG" "RCB2", build id, boots of
 *     this build that stopped in clock_init, the last stage clock_init_at() started, the last
 *     fault's exception number (24 = the watchdog: a hang), then pairs. A boot carries the last
 *     boot's fault (pc, lr, exception) into its own record, since a fault or watchdog reset is
 *     followed by a new boot that rewrites the record.
 * Anything unexpected turns it off for the rest of the boot; it never stops the boot.
 */
#include <stdint.h>

#include "bootcrumb.h"

#define REG(a)    (*(volatile uint32_t *)(a))
#define MMC       0x60030000u
#define M(o)      REG(MMC + (o))
#define FW_PHYS   0x2000u
#define FW2_LBA   0x11000u
#define CRUMB_LBA 0x10EA4u
#define TAG0      0x474C4245u              /* "EBLG" */
#define TAG_RC    0x31424352u              /* "RCB1" (+ sector) */
#define PAIRS     60u
#define TRIES     4000000u

enum { CTRL = 0x00, BLKSIZ = 0x1C, BYTCNT = 0x20, CMDARG = 0x28, CMD = 0x2C, RESP0 = 0x30,
       RINTSTS = 0x44, STATUS = 0x48, FIFO = 0x200 };
#define CMD_READ1  0xA0002351u
#define CMD_WRITE1 0xA0002758u
#define INT_ERRS   0xBFC2u

#define CRU_GATE1 0x40180084u              /* bit 9: clk_emmc */
#define CRU_GATE6 0x40180098u              /* bit 12: hclk_emmc */
#define CRU_SRST1 0x401800C4u              /* bit 1: the controller held in reset */

static uint32_t s_rec[2][128], s_sec[128];
static uint32_t s_mode;                    /* 0 off, 1 sector addressed, 2 byte addressed */
static uint32_t s_prev_step;
static uint32_t s_g1, s_g6, s_blksiz, s_bytcnt;

/* this build, so a hang counted against an older one never holds a new one back: a hash of the
 * loaded code and read-only data (0x03050000 .. _data_load_start, firmware_ap.ld). Deterministic,
 * unlike a build timestamp: the same source gives the same image, and the tests can tell it is. */
extern const uint32_t _data_load_start[];

static uint32_t build_id(void)
{
    static uint32_t id;
    uint32_t h = 0x811C9DC5u;

    if (id)
        return id;
    for (const uint32_t *p = (const uint32_t *)0x03050000u; p < _data_load_start; p++)
        h = (h ^ *p) * 0x01000193u;
    return id = h | 1u;
}

static int mmc_idle(void)
{
    uint32_t n = TRIES;

    while (M(STATUS) & 0x200u)
        if (!--n)
            return -1;
    if (!(M(STATUS) & 0x4u)) {
        M(CTRL) |= 2u;
        for (n = TRIES; M(CTRL) & 2u; )
            if (!--n)
                return -2;
    }
    M(RINTSTS) = 0xFFFFFFFFu;
    return 0;
}

static int mmc_cmd(uint32_t cmd, uint32_t arg)
{
    uint32_t n;

    if (mmc_idle())
        return -1;
    M(BLKSIZ) = 512u;
    M(BYTCNT) = 512u;
    M(CMDARG) = arg;
    M(CMD) = cmd;
    for (n = TRIES; M(CMD) & 0x80000000u; )
        if (!--n)
            return -3;
    for (n = TRIES; !(M(RINTSTS) & 0x4u); )
        if (!--n)
            return -4;
    return (M(RINTSTS) & 0x1142u) ? -5 : 0;
}

static int mmc_read(uint32_t arg, uint32_t *buf)
{
    uint32_t i = 0, n = TRIES;
    int rc = mmc_cmd(CMD_READ1, arg);

    if (rc)
        return rc;
    while (i < 128u) {
        if (!(M(STATUS) & 0x4u)) {
            buf[i++] = M(FIFO);
            n = TRIES;
        } else if ((M(RINTSTS) & INT_ERRS) || !--n) {
            return -6;
        }
    }
    for (n = TRIES; !(M(RINTSTS) & 0x8u); )
        if (!--n)
            return -7;
    return (M(RINTSTS) & INT_ERRS) ? -8 : 0;
}

static int mmc_write(uint32_t arg, const uint32_t *buf)
{
    uint32_t i = 0, n = TRIES;
    int rc = mmc_cmd(CMD_WRITE1, arg);

    if (rc)
        return rc;
    while (i < 128u) {
        if (!(M(STATUS) & 0x8u)) {
            M(FIFO) = buf[i++];
            n = TRIES;
        } else if ((M(RINTSTS) & INT_ERRS) || !--n) {
            return -6;
        }
    }
    for (n = TRIES; !(M(RINTSTS) & 0x8u); )
        if (!--n)
            return -7;
    if (M(RINTSTS) & INT_ERRS)
        return -8;
    for (n = TRIES; M(STATUS) & 0x200u; )
        if (!--n)
            return -9;
    return 0;
}

/* The controller as the ROM or our driver left it: clocks on for the access, everything the
 * driver relies on put back after. */
/* (drivers/emmc_rknanoc.c: a queued write the card is programming finishes first; weak, so the
 * tests that link this file alone need no driver) */
extern void emmc_quiesce(void) __attribute__((weak));

static int begin(void)
{
    if (emmc_quiesce)
        emmc_quiesce();
    if (REG(CRU_SRST1) & 2u)
        return -1;
    s_g1 = REG(CRU_GATE1);
    s_g6 = REG(CRU_GATE6);
    if ((s_g1 & 0x200u) || (s_g6 & 0x1000u)) {
        REG(CRU_GATE6) = 0x10000000u;
        REG(CRU_GATE1) = 0x02000000u;
        for (volatile uint32_t d = 0; d < 2000u; d++)
            ;
    }
    s_blksiz = M(BLKSIZ);
    s_bytcnt = M(BYTCNT);
    return 0;
}

static void end(void)
{
    M(RINTSTS) = 0xFFFFFFFFu;
    M(BLKSIZ) = s_blksiz;
    M(BYTCNT) = s_bytcnt;
    REG(CRU_GATE1) = 0x02000000u | (s_g1 & 0x200u);
    REG(CRU_GATE6) = 0x10000000u | (s_g6 & 0x1000u);
}

static uint32_t arg_of(uint32_t lba)
{
    return s_mode == 2u ? (FW_PHYS + lba) * 512u : FW_PHYS + lba;
}

static int is_fw_header(const uint32_t *s)
{
    return s[3] == 0x76543210u && s[0x1F8 / 4] == 0x616E4B52u && s[0x1FC / 4] == 0x57466F6Eu;
}

static int sector_zero(const uint32_t *s)
{
    for (unsigned i = 0; i < 128u; i++)
        if (s[i])
            return 0;
    return 1;
}

static void persist(void)
{
    if (!s_mode || begin())
        return;
    for (uint32_t i = 0; i < 2u; i++)
        if (mmc_write(arg_of(CRUMB_LBA + i), s_rec[i])) {
            s_mode = 0;                    /* a write failed: no more tries this boot */
            break;
        }
    end();
}

void bootcrumb_init(void)
{
    uint32_t boots = 0, prev_step = 0, prev_build = 0, hangs = 0, prev_stage = 0;
    uint32_t prev_faults = 0, prev_pc = 0, prev_lr = 0, prev_exc = 0;
    uint32_t carry_k[16], carry_v[16], ncarry = 0;

    s_mode = 0;
    if (begin())
        return;
    for (uint32_t m = 1; m <= 2u && !s_mode; m++) {
        s_mode = m;
        if (mmc_read(arg_of(0), s_sec) || !is_fw_header(s_sec))
            s_mode = 0;
    }
    if (s_mode && (mmc_read(arg_of(FW2_LBA), s_sec) || !is_fw_header(s_sec)))
        s_mode = 0;
    for (uint32_t i = 0; i < 2u && s_mode; i++) {
        if (mmc_read(arg_of(CRUMB_LBA + i), s_sec))
            s_mode = 0;
        else if (s_sec[0] == TAG0 && s_sec[1] == TAG_RC + i) {
            for (uint32_t w = 8u; w + 1u < 128u; w += 2u)   /* the kept keys, for the next record */
                if (s_sec[w] >= BCK_LOG_FLUSH && s_sec[w] < BCK_LOG_FLUSH + 16u && ncarry < 16u) {
                    carry_k[ncarry] = s_sec[w] + BCK_PREV;
                    carry_v[ncarry++] = s_sec[w + 1u];
                }
            if (!i) {
                boots = s_sec[2];
                prev_step = s_sec[3];
                prev_faults = s_sec[4];
                prev_pc = s_sec[6];
                prev_lr = s_sec[7];
            } else {
                prev_build = s_sec[2];
                hangs = s_sec[3];
                prev_stage = s_sec[4];
                prev_exc = s_sec[5];
            }
        } else if (!sector_zero(s_sec))
            s_mode = 0;
    }
    end();
    for (uint32_t i = 0; i < 2u; i++) {
        for (unsigned w = 0; w < 128u; w++)
            s_rec[i][w] = 0;
        s_rec[i][0] = TAG0;
        s_rec[i][1] = TAG_RC + i;
    }
    s_rec[0][2] = boots + 1u;
    s_rec[1][2] = build_id();
    s_rec[1][3] = prev_build == build_id() ? hangs + (prev_step == BC_CLOCK) : 0u;
    bootcrumb_kv(0x21, s_mode);            /* the addressing found (tools/bootlog.py: mmc_mode) */
    bootcrumb_kv(BCK_PREV_STEP, prev_step);
    s_prev_step = prev_step;
    bootcrumb_kv(BCK_CLOCK_HANGS, s_rec[1][3]);
    bootcrumb_kv(BCK_PREV_STAGE, prev_stage);   /* where the last boot was inside clock_init */
    for (uint32_t i = 0; i < ncarry; i++)
        bootcrumb_kv(carry_k[i], carry_v[i]);
    if (prev_faults) {
        bootcrumb_kv(BCK_PREV_FAULT_PC, prev_pc);
        bootcrumb_kv(BCK_PREV_FAULT_LR, prev_lr);
        bootcrumb_kv(BCK_PREV_FAULT_EXC, prev_exc);
    }
}

void bootcrumb_kv(uint32_t key, uint32_t val)
{
    uint32_t n = s_rec[0][5];

    if (n < 2u * PAIRS) {
        s_rec[n / PAIRS][8u + (n % PAIRS) * 2u] = key;
        s_rec[n / PAIRS][9u + (n % PAIRS) * 2u] = val;
        s_rec[0][5] = n + 1u;
    }
}

void bootcrumb_set(uint32_t key, uint32_t val)
{
    uint32_t n = s_rec[0][5];

    for (uint32_t i = 0; i < n; i++)
        if (s_rec[i / PAIRS][8u + (i % PAIRS) * 2u] == key) {
            s_rec[i / PAIRS][9u + (i % PAIRS) * 2u] = val;
            persist();
            return;
        }
    bootcrumb_kv(key, val);
    persist();
}

void bootcrumb(uint32_t step)
{
    s_rec[0][3] = step;
    persist();
}

uint32_t bootcrumb_step(void)
{
    return s_rec[0][3];
}

uint32_t bootcrumb_prev_step(void)
{
    return s_prev_step;
}

uint32_t bootcrumb_clock_hangs(void)
{
    return s_mode ? s_rec[1][3] : 0u;
}

void bootcrumb_clock_stage(uint32_t stage)
{
    s_rec[1][4] = stage;
    persist();
}

void bootcrumb_fault(uint32_t pc, uint32_t lr, uint32_t exc)
{
    s_rec[0][4]++;
    s_rec[0][6] = pc;
    s_rec[0][7] = lr;
    s_rec[1][5] = exc;
    bootcrumb_kv(0x30, exc);               /* FAULT_ipsr */
    bootcrumb_kv(0x33, REG(0xE000ED28u));  /* CFSR */
    bootcrumb_kv(0x34, REG(0xE000ED2Cu));  /* HFSR */
    bootcrumb_kv(0x35, REG(0xE000ED38u));  /* BFAR */
    persist();
}
