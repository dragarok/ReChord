/*
 * emmc_rknanoc.c -- eMMC on the RKNanoC DW-MMC controller (0x60030000),
 * polled.
 *
 * Clean-room implementation of FACTS extracted from the reference fw1
 * (init 0x030527ac, command 0x0305257c, data 0x030523d4, clock 0x0305235c,
 * block read 0x03052694 / write 0x0305271c, layout 0x03052a98; CRU gate
 * 0x030513e0 / reset 0x03051430).
 */
#include "services/watchdog.h"
#include <stdint.h>
#include <string.h>

#include "clock.h"
#include "emmc.h"
#include "rknanoc.h"

/* Two DW-MMC instances share the code below: eMMC 0x60030000, SD card
 * 0x60020000 (reference sd init 0x03062d84, read 0x03062a2c). */
static volatile uint32_t *s_mmc = (volatile uint32_t *)0x60030000u;
static int s_on_sd;
#define MMC  s_mmc
#define CARD_GATE (s_on_sd ? 0x6Bu : 0x6Cu)
#define CRU  RK_CRU
#define GRF  RK_GRF
#define delay_us  rk_delay_us
#define clk_gate  cru_gate
#define clk_reset cru_reset

enum {                       /* DW-MMC word offsets */
    CTRL = 0, PWREN = 1, CLKDIV = 2, CLKSRC = 3, CLKENA = 4, TMOUT = 5,
    CTYPE = 6, BLKSIZ = 7, BYTCNT = 8, INTMASK = 9, CMDARG = 10, CMD = 11,
    RESP0 = 12, RINTSTS = 17, STATUS = 18, FIFOTH = 19, FIFO = 0x80,
};

#define CMD_START    0x80000000u
#define STATUS_BUSY  0x200u          /* card data busy */
#define STATUS_EMPTY 0x4u            /* FIFO empty */
#define F_RESP       0x40u
#define F_LONG       0x80u
#define F_CRC        0x100u
#define F_STOP       0x4000u
#define F_INIT       0x8000u
#define RCA_ARG      0x20000u        /* RCA 2 */

static uint32_t s_sectors, s_fifo_words = 16u;
static uint32_t s_rca = RCA_ARG;     /* the internal part's RCA (an SD NAND publishes its own) */
static uint32_t s_shift;             /* card address = LBA << s_shift: 9 = byte addressed */
static uint32_t s_blksz = 512u;      /* data block size (the bus test uses 4 or 8) */
static int s_width = 1;              /* data lines in use */
static const char *s_kind = "none";
static uint32_t s_write_floor = 0xFFFFFFFFu;   /* set by emmc_layout() */
static uint32_t s_fw_sectors;                   /* ditto, 0 = no fw writes */

static void select_mmc(int sd)
{
    s_on_sd = sd;
    s_mmc = (volatile uint32_t *)(sd ? 0x60020000u : 0x60030000u);
}

/* The microSD card is out (card detect GPIO0.11 reads high, its pull-up: reference 0x03062c34):
 * every polled wait on the SD instance gives up at once instead of running into its timeout. */
static int sd_pin_out(void)
{
    return (((volatile uint32_t *)0x40160000u)[0x50 / 4] >> 11) & 1u;
}

static int sd_out(void)
{
    return s_on_sd && sd_pin_out();
}

static int wait_clear(unsigned reg, uint32_t mask, uint32_t tries)
{
    while (MMC[reg] & mask) {
        if (tries-- == 0 || sd_out())
            return -1;
        delay_us(1);
    }
    return 0;
}

static int update_clock(void)
{
    MMC[CMD] = 0x80202000u;          /* start | update clock registers only */
    return wait_clear(CMD, CMD_START, 50000);
}

/* Card clock from the 24 MHz source: CRU pre-divider, then CLKDIV. */
static int set_clock(uint32_t khz)
{
    uint32_t div = (khz + 23999u) / khz, cd;

    if (div != 1u) {
        if (div & 1u)
            div++;
        if (div > 31u)
            div = 32u;
    }
    cd = (24000u / div + khz - 1u) / khz;
    if (cd > 1u && (cd & 1u))
        cd++;
    MMC[CLKENA] = 0;
    if (update_clock())
        return -1;
    CRU[0x30 / 4] = s_on_sd ? 0x009F0080u | (div - 1u)        /* SD: low half */
                            : 0x9F008000u | (div - 1u) << 8;
    MMC[CLKDIV] = cd >> 1;
    if (update_clock())
        return -1;
    MMC[CLKENA] = 0x10001u;
    return update_clock();
}

static int send_cmd(uint32_t idx, uint32_t arg, uint32_t flags, int wait_busy)
{
    uint32_t tries = 1000001u, st;

    if (wait_clear(STATUS, STATUS_BUSY, 500000))
        return -2;
    MMC[RINTSTS] = 0xFFFFFFFFu;
    MMC[CMDARG] = arg;
    MMC[CMD] = idx | flags | 0xA0002000u;
    if (wait_clear(CMD, CMD_START, 50000))
        return -3;
    while (!(MMC[RINTSTS] & 0x4u)) {        /* command done */
        if (--tries == 0 || sd_out())
            return -4;
        delay_us(1);
    }
    st = MMC[RINTSTS];
    if (st & 0x1102u)                       /* resp error / timeout / HLE */
        return -5;
    if ((flags & F_CRC) && (st & 0x40u))    /* resp CRC */
        return -6;
    if (wait_busy && wait_clear(STATUS, STATUS_BUSY, 1000000))
        return -7;
    return 0;
}

/* A data command in two halves: xfer_start() sends it and moves the data through the FIFO,
 * xfer_finish() waits for data over (a write's DTO comes once the card has the block; the card may
 * then still program it, STATUS_BUSY). The write-behind queue below runs the halves apart. */
static int xfer_start(uint32_t idx, uint32_t arg, uint32_t *buf, uint32_t len, int write)
{
    uint32_t words = len >> 2, fifo_tries = 5000000u;

    if (wait_clear(STATUS, STATUS_BUSY, 1000000))
        return -2;
    if (!(MMC[STATUS] & STATUS_EMPTY)) {
        MMC[CTRL] |= 2u;                    /* FIFO reset */
        wait_clear(CTRL, 2u, 5000);
    }
    MMC[BLKSIZ] = s_blksz;
    MMC[BYTCNT] = len;
    MMC[RINTSTS] = 0xFFFFFFFFu;
    MMC[CMDARG] = arg;
    MMC[CMD] = idx | (write ? 0x400u : 0u) | 0xA0002340u;  /* data, CRC, resp */
    if (wait_clear(CMD, CMD_START, 50000))
        return -3;
    while (words) {
        uint32_t n;

        if (MMC[RINTSTS] & 0xAF82u)
            return -12;
        n = (MMC[STATUS] & 0x3FFFFFFFu) >> 17;  /* words in FIFO */
        if (write)
            n = n < s_fifo_words ? s_fifo_words - n : 0u;
        if (n == 0u) {
            if (--fifo_tries == 0u || sd_out())
                return -13;
            delay_us(1);
            continue;
        }
        if (n > words)
            n = words;
        words -= n;
        if (write)
            while (n--)
                MMC[FIFO] = *buf++;
        else
            while (n--)
                *buf++ = MMC[FIFO];
    }
    return 0;
}

static int xfer_finish(void)
{
    uint32_t tries = 5000001u;
    int ret = 0;

    while (!(MMC[RINTSTS] & 0xAE88u)) {     /* data over or error */
        if (--tries == 0u || sd_out()) {
            ret = -10;
            break;
        }
        delay_us(1);
    }
    if (!ret && (MMC[RINTSTS] & 0xAE80u))
        ret = -11;
    MMC[RINTSTS] = 0xFFFFFFFFu;
    return ret;
}

static int xfer_data(uint32_t idx, uint32_t arg, uint32_t *buf, uint32_t len, int write)
{
    int ret = xfer_start(idx, arg, buf, len, write);

    return ret ? ret : xfer_finish();
}

static uint32_t s_sd_rca;           /* the SD card's RCA (it publishes it with CMD3) */
static uint32_t s_sd_sectors, s_sd_wr;
static int s_sd_hc, s_sd_4bit, s_cd_ready;


static int wait_tran(void)
{
    for (uint32_t i = 0; i < 100000u; i++) {
        uint32_t r;

        if (send_cmd(13, s_on_sd ? s_sd_rca << 16 : s_rca, F_RESP | F_CRC, 0))   /* (the selected card's) */
            return -1;
        r = MMC[RESP0];
        if ((r & 0x100u) && ((r & 0x1FFFu) >> 9) == 4u)   /* ready, tran */
            return 0;
        delay_us(10);
    }
    return -2;
}

/* CSD 1.x capacity: (C_SIZE + 1) << (C_SIZE_MULT + 2) blocks of 2^READ_BL_LEN bytes. */
static uint32_t csd_v1_sectors(const uint32_t *csd)
{
    uint32_t c_size = ((csd[2] & 0x3FFu) << 2 | csd[1] >> 30) + 1u;
    uint32_t mult = (csd[1] >> 15) & 7u, bl = (csd[2] >> 16) & 15u;

    return (uint32_t)(((uint64_t)c_size << (mult + 2u) << bl) >> 9);
}

static uint32_t s_idb1[128], s_idbw[128];   /* sector 0x40 at 1 bit, then at the new width */

static int block_read(uint32_t arg, uint32_t count, void *buf);

/* Read sector 0x40 (the IDB) at the bus width now set; the card clock stays on (init). */
static int read_idb(uint32_t *buf)
{
    int r = block_read(0x40u << s_shift, 1, buf);

    clk_gate(CARD_GATE, 1);
    return r;
}

/* JEDEC bus test: CMD19 sends a pattern, CMD14 reads it back inverted; a data line
 * the part does not have reads as 1 and spoils it. */
static int bus_test(int width)
{
    uint32_t out[2] = { 0, 0 }, in[2] = { 0, 0 }, n = width == 8 ? 8u : 4u;
    uint8_t *o = (uint8_t *)out, *q = (uint8_t *)in;
    int r;

    if (width == 8) {
        o[0] = 0x55;
        o[1] = 0xAA;
    } else {
        o[0] = 0x5A;
    }
    MMC[CTYPE] = width == 8 ? 0x10000u : 1u;
    s_blksz = n;
    r = xfer_data(19, 0, out, n, 1);
    if (!r)
        r = xfer_data(14, 0, in, n, 0);
    s_blksz = 512u;
    MMC[CTYPE] = 0;
    if (r)
        return -1;
    return (uint8_t)(q[0] ^ o[0]) == 0xFFu && (width != 8 || (uint8_t)(q[1] ^ o[1]) == 0xFFu) ? 0 : -1;
}

/* Switch an eMMC to width (8 or 4) if the bus test passes and the IDB reads back the
 * same as at 1 bit; otherwise stay at 1 bit. 0 = switched. */
static int set_width(int width)
{
    if (read_idb(s_idb1) || bus_test(width) ||
        send_cmd(6, width == 8 ? 0x03B70200u : 0x03B70100u, F_RESP | F_CRC, 1) || wait_tran())
        return -1;
    MMC[CTYPE] = width == 8 ? 0x10000u : 1u;
    if (read_idb(s_idbw) == 0 && !memcmp(s_idb1, s_idbw, sizeof s_idbw)) {
        s_width = width;
        return 0;
    }
    MMC[CTYPE] = 0;                         /* back to 1 bit, on both ends */
    send_cmd(6, 0x03B70000u, F_RESP | F_CRC, 1);
    wait_tran();
    return -1;
}

/* SD-protocol NAND on the internal port (stock handles these too): SDSC parts are
 * byte addressed. 4-bit if a read-back agrees, else 1 bit. v2: it answered CMD8 (SD 2.0+);
 * an SD 1.x part did not, and gets ACMD41 without HCS (stock: SDXX_Init(FALSE)). */
static int sdnand_init(int v2)
{
    uint32_t i, csd[4];
    int hc;

    for (i = 0; ; i++) {
        if (send_cmd(55, 0, F_RESP | F_CRC, 0))
            return -20;
        if (send_cmd(41, v2 ? 0x40FF8000u : 0x00FF8000u, F_RESP, 0) == 0 && (MMC[RESP0] & 0x80000000u))
            break;
        if (i == 1000u)
            return -21;
        delay_us(1000);
    }
    hc = v2 && (MMC[RESP0] & 0x40000000u) != 0;
    s_shift = hc ? 0u : 9u;
    if (send_cmd(2, 0, F_RESP | F_LONG | F_CRC, 0) || send_cmd(3, 0, F_RESP | F_CRC, 0))
        return -22;
    s_rca = MMC[RESP0] & 0xFFFF0000u;
    if (!s_rca || send_cmd(9, s_rca, F_RESP | F_LONG | F_CRC, 0))
        return -23;
    for (i = 0; i < 4u; i++)
        csd[i] = MMC[RESP0 + i];
    if (csd[3] >> 30 == 0u)
        s_sectors = csd_v1_sectors(csd);
    else
        s_sectors = (((csd[2] & 0x3Fu) << 16 | csd[1] >> 16) + 1u) << 10;
    if (s_sectors < 0x20000u)
        return -24;
    if (set_clock(24000) || send_cmd(7, s_rca, F_RESP | F_CRC, 1))
        return -25;
    if (!hc && send_cmd(16, 512, F_RESP | F_CRC, 0))
        return -26;
    s_kind = hc ? "SD NAND (SDHC)" : v2 ? "SD NAND (SDSC)" : "SD NAND (SD 1.x)";
    if (read_idb(s_idb1) == 0 && send_cmd(55, s_rca, F_RESP | F_CRC, 0) == 0 &&
        send_cmd(6, 2, F_RESP | F_CRC, 0) == 0) {           /* ACMD6: 4-bit */
        MMC[CTYPE] = 1u;
        if (read_idb(s_idbw) == 0 && !memcmp(s_idb1, s_idbw, sizeof s_idbw)) {
            s_width = 4;
        } else {
            MMC[CTYPE] = 0;
            send_cmd(55, s_rca, F_RESP | F_CRC, 0);
            send_cmd(6, 0, F_RESP | F_CRC, 0);
        }
    }
    clk_gate(0x6C, 0);
    return 0;
}

const char *emmc_kind(void) { return s_kind; }
int emmc_width(void)        { return s_width; }

int emmc_init(void)
{
    static uint32_t ext_csd[128];
    uint32_t i, csd[4];

    select_mmc(0);
    clk_gate(0x19, 1);
    clk_gate(0x6C, 1);
    clk_reset(0x11, 0);
    CRU[0x30 / 4] = 0x9F009F00u;
    GRF[0] = 0xFFFF5555u;                   /* eMMC pinmux */
    GRF[1] = 0x003F0015u;
    GRF[9] = 0x00C00040u;
    delay_us(10);

    MMC[CTRL] = 7u;                         /* controller/FIFO/DMA reset */
    if (wait_clear(CTRL, 7u, 1000))
        return -1;
    i = (MMC[FIFOTH] & 0x0FFFFFFFu) >> 16;  /* reset RX watermark = depth-1 */
    s_fifo_words = (i - 7u < 0x3F9u) ? i + 1u : 16u;
    MMC[FIFOTH] = 0x300F0010u;
    MMC[CTYPE] = 0;
    MMC[CLKSRC] = 0;
    MMC[TMOUT] = 0xFFFFFF40u;
    MMC[RINTSTS] = 0xFFFFFFFFu;
    MMC[INTMASK] = 0;
    MMC[PWREN] = 1;
    if (set_clock(400))
        return -2;
    MMC[PWREN] = 0;
    delay_us(10);
    MMC[PWREN] = 1;
    delay_us(600);

    send_cmd(0, 0, F_INIT, 0);
    delay_us(100);
    /* an SD-protocol NAND answers CMD8, an eMMC does not (stock probes in this order too) */
    if (send_cmd(8, 0x1AAu, F_RESP | F_CRC, 0) == 0 && (MMC[RESP0] & 0xFFFu) == 0x1AAu)
        return sdnand_init(1);
    /* no CMD8: an SD 1.x part still knows APP_CMD, an eMMC does not (stock's _IdentifyUnknow probes
     * CMD55 before falling back to MMC's CMD1, which an SD card never answers) */
    send_cmd(0, 0, F_INIT, 0);
    delay_us(100);
    if (send_cmd(55, 0, F_RESP | F_CRC, 0) == 0)
        return sdnand_init(0);
    send_cmd(0, 0, F_INIT, 0);
    delay_us(100);
    for (i = 0; ; i++) {
        int r;

        if (i == 8000u)
            return -3;
        r = send_cmd(1, 0x40FF8000u, F_RESP, 0);
        if (r == 0 && (MMC[RESP0] & 0x80000000u))
            break;
        if (r == -3 || r == -4)             /* controller, not card, stuck */
            return -15;
        delay_us(1000);
    }
    s_shift = (MMC[RESP0] & 0x40000000u) ? 0u : 9u;   /* 2 GB and less: byte addressed */
    s_rca = RCA_ARG;
    if (send_cmd(2, 0, F_RESP | F_LONG | F_CRC, 0))
        return -5;
    if (send_cmd(3, RCA_ARG, F_RESP | F_CRC, 0))
        return -6;
    if (send_cmd(9, RCA_ARG, F_RESP | F_LONG | F_CRC, 0))
        return -7;
    for (i = 0; i < 4u; i++)
        csd[i] = MMC[RESP0 + i];
    if (set_clock(24000))
        return -8;
    if (send_cmd(7, RCA_ARG, F_RESP | F_CRC, 1))
        return -9;
    if (send_cmd(16, 512, F_RESP | F_CRC, 0))
        return -10;
    if (((csd[3] >> 26) & 0xFu) < 4u) {     /* SPEC_VERS < 4: no EXT_CSD, SWITCH or bus test (stock skips them too) */
        s_sectors = csd_v1_sectors(csd);
        if (s_sectors < 0x20000u)
            return -12;
        s_kind = "MMC (before 4.0)";
        clk_gate(0x6C, 0);
        return 0;                           /* (1-bit bus) */
    }
    if (xfer_data(8, 0, ext_csd, 512, 0))   /* EXT_CSD */
        return -11;
    s_sectors = ext_csd[212 / 4];           /* SEC_COUNT, 0 on parts of 2 GB and less */
    if (s_shift || !s_sectors)
        s_sectors = csd_v1_sectors(csd);
    if (s_sectors < 0x20000u)               /* under 64 MiB: not this player's storage */
        return -12;
    s_kind = s_shift ? "eMMC, byte addressed" : "eMMC";
    if (set_width(8))                       /* the widest the bus test and a read-back agree on */
        set_width(4);
    clk_gate(0x6C, 0);
    return 0;
}

uint32_t emmc_sectors(void)
{
    return s_sectors;
}

/* A FIFO reset, with the card's clock on (an access with it gated gets no bus answer: QEMU's CRU
 * checker, SDK Hw_cru.h; the read retry used to do it after the gate went off). */
static void fifo_reset(void)
{
    MMC[CTRL] |= 2u;
    wait_clear(CTRL, 2u, 1000);
}

/* CMD17/CMD18 (+CMD12) on the selected instance; arg = card address. */
static int block_read_once(uint32_t arg, uint32_t count, void *buf)
{
    int ret, stop;

    clk_gate(CARD_GATE, 1);
    if (count == 1u) {
        ret = xfer_data(17, arg, buf, 512, 0);
    } else {
        ret = xfer_data(18, arg, buf, count << 9, 0);
        stop = send_cmd(12, s_rca, F_RESP | F_CRC | F_STOP, 1);
        if (!ret)
            ret = stop;
    }
    if (ret)
        fifo_reset();                       /* (for the retry: while the clock is still on) */
    clk_gate(CARD_GATE, 0);
    return ret;
}

/* A failed read is retried twice after a FIFO reset: one transient error must not
 * end the track (or the playlist) being streamed. */
static int block_read(uint32_t arg, uint32_t count, void *buf)
{
    int ret = block_read_once(arg, count, buf);

    watchdog_feed();             /* (long storage work keeps it fed, as stock's FS code does) */
    for (int i = 0; ret && i < 2; i++)     /* (block_read_once reset the FIFO) */
        ret = block_read_once(arg, count, buf);
    return ret;
}

/* ---- the write-behind queue (Internal and the microSD card) ----------------
 *
 * A single-block write costs the UI nothing but the transfer: emmc_write_async() / sd_write_async()
 * copy the block into one queue and return; emmc_wq_step(), called once per main-loop pass, sends
 * the oldest one on its own instance (CMD24 and the FIFO, xfer_start) and on a later pass sees it
 * done (data over, STATUS_BUSY clear -- the card programmed it -- then CMD13 in tran, the same
 * signals send_cmd()/wait_tran() and bootcrumb's polled writes wait on). The card is programming for
 * most of a write (the device's 600 ms log appends were about 8 of them, OURS_AUDIT 1.6; an SD card
 * may take up to 250 ms, 500 ms SDXC: SD Physical Layer Simplified 4.6.2.2), and nothing waits for
 * that any more.
 *
 * Order is kept, across both devices: blocks reach the cards oldest first (one block in flight at a
 * time), a block queued again while it is the newest (same device) is replaced in place, which
 * skips only a state the next one overwrites. So whatever stops the queue -- a power cut, the card
 * pulled, the card failing -- leaves each card holding a prefix of the writes the file system made,
 * the states fs_target.c / fs_write_ext.c's crash order (data, FAT, entry) is built for (and
 * tests/fs_sdrw.py cuts at every write). A card block that fails stops the card's queue there:
 * nothing written after a write that did not take (sd_fail).
 *
 * Coherent: emmc_read() / sd_read() let a write in flight on their own instance finish (the card
 * cannot be read while it programs; the other instance is not waited for), then serve queued blocks
 * from the queue; emmc_write()/emmc_fw_write()/sd_write() (USB disk, the updater, big writes) drain
 * the queue first; emmc_quiesce() lets bootcrumb.c's own register writes in. emmc_sync() drains it
 * (power off, sessions, reboot); sd_sync() is the card's durability point (USB disk, a list moved
 * to the card). */
#ifndef EMMC_WQ_SLOTS
#define EMMC_WQ_SLOTS 16u
#endif
/* A card's block not programmed this long after it was sent: the card has failed. The SD spec's
 * write busy limit is 250 ms (500 ms SDXC, 4.6.2.2); sd-rw's synchronous driver rode out 3 s (m44
 * busyok: its 1 s busy waits and two retries), which this keeps, so a slow card in spec is never
 * dropped. */
#define WQ_SD_MS  3500u
/* A wait that holds the loop (a full queue, a sync, a read) with the card-detect pin reading "out":
 * past sdcard.c's debounce (three samples 100 ms apart) the card is taken as gone. */
#define WQ_OUT_MS 400u
static struct {                          /* (one struct: one base address per function, -Os) */
    unsigned head, n;                    /* the oldest block, blocks queued (the head may be in flight) */
    int flight;                          /* 1 + the head's device while it is sent and programmed */
    unsigned fl;                         /* the head's run: blocks sent in one command (or to be popped) */
    uint32_t t0;                         /* time_ms() when it was sent (a stuck card: wq_done) */
    unsigned tries;                      /* the card's head block: failed sends */
    emmc_wq_stats_t st;
    uint32_t lba[EMMC_WQ_SLOTS];
    uint8_t dev[EMMC_WQ_SLOTS];          /* 0 Internal (eMMC), 1 the card */
    uint32_t buf[EMMC_WQ_SLOTS][128];
} Q;

static int raw_write(uint32_t lba, uint32_t count, const void *buf);
static int wq_done(int wait);

/* A block's command argument on its device: SDSC cards are byte addressed (SD.c:601-604). */
static uint32_t card_arg(int dev, uint32_t lba)
{
    return dev ? (s_sd_hc ? lba : lba << 9) : lba << s_shift;
}

/* A queued command's end on the selected instance: after a failure the FIFO reset and the status
 * cleared (while the clock is on: the CRU checker), then the card's clock gated. */
static void xfer_end(int r)
{
    if (r) {
        fifo_reset();
        MMC[RINTSTS] = 0xFFFFFFFFu;
    }
    clk_gate(CARD_GATE, 0);
}

/* The head's run (Q.fl blocks) off the queue; rc: it failed (counted). */
static void wq_pop(int rc)
{
    if (rc) {
        Q.st.errors++;
        Q.st.last_error = rc;
    }
    Q.st.written += Q.fl;
    Q.head = (Q.head + Q.fl) % EMMC_WQ_SLOTS;
    Q.n -= Q.fl;
}

/* The card's blocks dropped (it went, or failed), counted; Internal's written now, in their order
 * (the slow way: this is rare), and the queue is empty. */
static void wq_drop_sd(void)
{
    if (Q.flight == 1)
        wq_done(1);
    Q.flight = 0;                            /* (the card's: never on the paths here -- sd_fail runs
                                              * with none in flight -- and sd_init resets the controller) */
    for (; Q.n; Q.n--, Q.head = (Q.head + 1u) % EMMC_WQ_SLOTS) {
        if (Q.dev[Q.head])
            Q.st.dropped++;
        else if (raw_write(Q.lba[Q.head], 1, Q.buf[Q.head]))
            Q.st.errors++;
    }
    Q.tries = 0;
}

/* The card failed: nothing more is written to it (its queued blocks dropped, 0 sectors: every
 * read and write refused); services/sdcard.c unmounts it (emmc_wq_stats()->sd_fail). */
static void __attribute__((noinline)) sd_fail(int code)
{
    if (Q.st.sd_fail)                        /* (the first reason stays: a big write that found the
                                              * card failed must not make it "removed") */
        return;
    Q.st.errors++;
    Q.st.last_error = code;
    Q.st.sd_fail = code;
    s_sd_sectors = 0;                        /* (the USB disk shows no medium) */
    wq_drop_sd();
}

/* The head's run sent: the blocks after it on the same device with the next LBAs (up to the end of
 * the ring: their buffers follow each other) in one command -- CMD24 for one, CMD25 for several,
 * ended by CMD12 in wq_done (block_write_once's sequence) -- then back to the caller while the card
 * programs them. 0 = sent; 1 = not sent: the card's detect pin reads "out" (it waits); < 0 = failed. */
static int wq_send(void)
{
    unsigned h = Q.head, k = 1;
    int dev = Q.dev[h], r;

    while (k < Q.n && h + k < EMMC_WQ_SLOTS && Q.dev[h + k] == dev && Q.lba[h + k] == Q.lba[h] + k)
        k++;
    Q.fl = k;
    watchdog_feed();
    select_mmc(dev);
    if (sd_out())
        return 1;
    clk_gate(CARD_GATE, 1);
    if ((r = xfer_start(k > 1u ? 25 : 24, card_arg(dev, Q.lba[h]), Q.buf[h], k << 9, 1))) {
        xfer_end(r);
        return r;
    }
    Q.flight = 1 + dev;
    Q.t0 = time_ms();
    return 0;
}

/* The head's run failed (r): sent again, twice (after a FIFO reset, as reads are retried), unless
 * it is the card's and the card is out (then it waits) or stuck. Then Internal's is counted and
 * dropped (as it always was: emmc_sync reports it), the card has failed. */
static void wq_failed(int r)
{
    int dev = Q.dev[Q.head];

    if ((dev && sd_pin_out()) || (Q.tries += r == -33 ? 3u : 1u) <= 2u)
        return;
    Q.tries = 0;
    if (dev)
        sd_fail(r);
    else
        wq_pop(r);
}

/* The block in flight: 1 = done (or failed: handled), 0 = the card still has it. wait: until done
 * (Internal: as long as it takes; the card: WQ_SD_MS from the send at most). */
static int wq_done(int wait)
{
    int r = 0, dev = Q.flight - 1;

    select_mmc(dev);                         /* (left selected: every entry point selects its own) */
    while (!(MMC[RINTSTS] & 0xAE88u) || (MMC[STATUS] & STATUS_BUSY)) {   /* data not over, or programming */
        if (sd_out()) {
            r = SD_ERR_NOCARD;               /* broken off: sent again when the pin reads it in */
            break;
        }
        if (time_ms() - Q.t0 >= (dev ? WQ_SD_MS : 2000u)) {
            Q.st.stuck++;                    /* Internal: 2 s, waited out below; the card: failed */
            r = dev ? -33 : 0;
            break;
        }
        if (!wait)
            return 0;
        watchdog_feed();
        delay_us(10);
    }
    if (!r)
        r = xfer_finish();
    if (Q.fl > 1u && r > -33) {              /* a CMD25 ended (also after a data error: the card back
                                              * to tran); not to a card stuck busy or gone */
        int s = send_cmd(12, s_rca, F_RESP | F_CRC | F_STOP, 1);   /* (SD: stuff bits, spec 4.7.4) */

        if (!r)
            r = s;
    }
    if (!r && wait_tran())
        r = -31;
    xfer_end(r);
    Q.flight = 0;
    if (r) {
        wq_failed(r);
    } else {
        Q.tries = 0;
        wq_pop(0);
    }
    return 1;
}

/* Holding the loop: the head block sent (if need be) and done, or handled. A card whose pin reads
 * "out" for WQ_OUT_MS meanwhile is gone (its blocks dropped). */
static void wq_head(void)
{
    uint32_t t = time_ms();
    int r;

    while (!Q.flight && Q.n && (r = wq_send())) {
        if (r < 0) {
            wq_failed(r);
            return;
        }
        if (time_ms() - t >= WQ_OUT_MS) {
            sd_fail(SD_ERR_NOCARD);
            return;
        }
        delay_us(100);
    }
    if (Q.flight)
        wq_done(1);
}

void emmc_quiesce(void)
{
    if (Q.flight == 1) {                     /* (the card's instance is not bootcrumb's) */
        Q.st.waits++;
        wq_done(1);
    }
}

int emmc_wq_step(int may_start)
{
    int r, f = Q.flight;

    /* Internal's run done: the next waits for the next step, so the audio refill reads in between.
     * The card's: the next goes at once (a track on the card is refilled when its headroom runs
     * low: may_start is audio_write_ok, ap_bg.c), else the card would get a block a pass. */
    if (f && (!wq_done(0) || f == 1))
        return (int)Q.n;
    if (may_start && Q.n && (r = wq_send()) < 0)
        wq_failed(r);
    return (int)Q.n;
}

int emmc_busy(void)
{
    return Q.flight;
}

unsigned emmc_wq_pending(void)
{
    return Q.n;
}

unsigned emmc_wq_free(void)
{
    return EMMC_WQ_SLOTS - Q.n;
}

const emmc_wq_stats_t *emmc_wq_stats(void)
{
    return &Q.st;
}

int emmc_sync(void)
{
    uint32_t errors = Q.st.errors + Q.st.dropped;

    if (Q.n)
        Q.st.syncs++;
    while (Q.n)
        wq_head();
    return Q.st.errors + Q.st.dropped != errors ? -32 : 0;
}

/* The blocks copied into the queue (dev 0 Internal, 1 the card); 0, or SD_ERR_NOCARD when the card
 * failed while this waited for room. */
static int wq_put(int dev, uint32_t lba, uint32_t count, const uint32_t *src)
{
    for (uint32_t i = 0; i < count; i++, src += 128) {
        unsigned tail = (Q.head + Q.n - 1u) % EMMC_WQ_SLOTS;

        if (Q.n && Q.lba[tail] == lba + i && Q.dev[tail] == dev && !(Q.flight && Q.n <= Q.fl)) {
            Q.st.merged++;                   /* the newest block again: replaced in place */
        } else {
            while (Q.n == EMMC_WQ_SLOTS) {   /* full: the oldest goes out first */
                Q.st.waits++;
                wq_head();
            }
            if (dev && Q.st.sd_fail)
                return SD_ERR_NOCARD;
            tail = (Q.head + Q.n++) % EMMC_WQ_SLOTS;
            Q.lba[tail] = lba + i;
            Q.dev[tail] = (uint8_t)dev;
            Q.st.queued++;
            if (Q.n > Q.st.peak)
                Q.st.peak = Q.n;
        }
        memcpy(Q.buf[tail], src, 512);
    }
    return 0;
}



/* A read on its instance (dev): a write in flight there waited for first (the card cannot be read
 * while it programs; Internal: as long as it takes, the card: WQ_SD_MS at most, then it has
 * failed), counted with its caller (lr); then the queued blocks, newer than the card's, copied over
 * what was read. A card pulled: SD_ERR_NOCARD, no bus traffic (the reader waits, no zeros). */
static int q_read(int dev, uint32_t lba, uint32_t count, void *buf, uint32_t lr)
{
    int r;

    if (Q.flight == 1 + dev) {
        Q.st.waits++;
        Q.st.wait_lr = lr;
        Q.st.wait_ms = time_ms();
        wq_done(1);
        Q.st.wait_ms = time_ms() - Q.st.wait_ms;
    }
    select_mmc(dev);
    if (sd_out() || (dev && Q.st.sd_fail))
        return SD_ERR_NOCARD;
    if ((r = block_read(card_arg(dev, lba), count, buf)))
        return sd_out() ? SD_ERR_NOCARD : r;
    for (unsigned k = 0; k < Q.n; k++) {
        unsigned i = (Q.head + k) % EMMC_WQ_SLOTS;

        if (Q.dev[i] == dev && Q.lba[i] - lba < count)
            memcpy((uint8_t *)buf + (Q.lba[i] - lba) * 512u, Q.buf[i], 512);
    }
    return 0;
}

int emmc_read(uint32_t lba, uint32_t count, void *buf)
{
    if (count == 0u)
        return 0;
    if (lba + count < lba || lba + count > s_sectors)
        return -30;
    return q_read(0, lba, count, buf, (uint32_t)__builtin_return_address(0));
}

int emmc_write_async(uint32_t lba, uint32_t count, const void *buf)
{
    if (count == 0u)
        return 0;
    if (lba < s_write_floor || lba + count < lba || lba + count > s_sectors)
        return -30;
    if (count > EMMC_WQ_SLOTS / 2u) {        /* a big write (a screenshot): one CMD25 after the queue */
        emmc_sync();
        return raw_write(lba, count, buf);
    }
    return wq_put(0, lba, count, buf);
}


/* CMD24 / CMD25 + CMD12 on the selected instance (arg = card address), then the card's programming
 * waited out: STATUS data_busy (send_cmd before CMD12 and before the next command) and CMD13 until
 * READY_FOR_DATA in tran (wait_tran) -- the eMMC write the reference does (0x0305271c) and the SD
 * spec's (Physical Layer Simplified 4.3.4: an open-ended CMD25 ends with CMD12; 4.6.2.2: busy up to
 * 250 ms, 500 ms on SDXC, inside wait_clear's bound). */
static int block_write_once(uint32_t arg, uint32_t count, const void *buf)
{
    int ret, r;

    clk_gate(CARD_GATE, 1);
    if (count == 1u) {
        ret = xfer_data(24, arg, (uint32_t *)buf, 512, 1);
    } else {
        ret = xfer_data(25, arg, (uint32_t *)buf, count << 9, 1);
        r = send_cmd(12, s_rca, F_RESP | F_CRC | F_STOP, 1);   /* (SD: stuff bits, spec 4.7.4) */
        if (!ret)
            ret = r;
    }
    r = wait_tran();                        /* programming done */
    if (!ret && r)
        ret = -31;
    if (ret)
        fifo_reset();                       /* (for the retry: while the clock is still on) */
    clk_gate(CARD_GATE, 0);
    return ret;
}

static int raw_write(uint32_t lba, uint32_t count, const void *buf)
{
    watchdog_feed();
    if (Q.flight == 1)                    /* (the queue's own retry comes here with none) */
        wq_done(1);
    select_mmc(0);
    return block_write_once(lba << s_shift, count, buf);
}

/* Refuses anything below the user volume: IDB and firmware slots are
 * never writable from here (also not before emmc_layout() succeeded). */
int emmc_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (count == 0u)
        return 0;
    if (lba < s_write_floor || lba + count < lba || lba + count > s_sectors)
        return -30;
    emmc_sync();                             /* the queued blocks are older: first */
    return raw_write(lba, count, buf);
}

uint32_t emmc_fw_sectors(void)
{
    return s_fw_sectors;
}

/* Firmware area only: [EMMC_FW_BASE, EMMC_FW_BASE + emmc_fw_sectors()). */
int emmc_fw_write(uint32_t rel, uint32_t count, const void *buf)
{
    if (count == 0u || rel + count < rel || rel + count > s_fw_sectors)
        return -30;
    emmc_sync();
    return raw_write(EMMC_FW_BASE + rel, count, buf);
}

/* ---- SD card ----
 *
 * Read and write, as stock writes it (MINIV390: MDWrite 0x03069422 -> SDCardWrite 0x030605c8 ->
 * _SDMMC_Write 0x030604f8 -> SDC_Request 0x0305eda4; the SDK's SD_MMC/SDM.c:147-222 and
 * SDCtrl.c:971-1229; the flag words are in all 26 stock versions):
 *   - one block CMD24, several CMD25 then CMD12: the CMD words 0xA0002758 / 0xA0002759 (start,
 *     use_hold, wait_prvdata, write, data, CRC, response: stock 0x0306054a / 0x03060528) are
 *     xfer_start()'s 0x400 | 0xA0002340 | idx; no auto-stop, no CMD23, no ACMD23 pre-erase (stock
 *     sends none; SDConfig.h:42 PRE_DEFINE_BLKCNT off). Stock's CMD12 (0x414C) leaves out
 *     wait_prvdata; ours keeps send_cmd()'s, as the read path's CMD12 always has (device-proven).
 *   - BLKSIZ 512, BYTCNT n * 512 (SDCtrl.c:1040-1041), FIFOTH 0x300F0010 and TMOUT 0xFFFFFF40 at
 *     init (stock 0x030602e4, SDCtrl.c:846-851): the eMMC values below.
 *   - the card's programming busy: stock polls STATUS bit 9 before the *next* command, 5,000,000 x
 *     1 us (SDCtrl.c:212-241, stock 0x0305eda4); ours polls it before every command (send_cmd,
 *     xfer_start: 0.5 / 1 s) and after the write waits for CMD13 READY_FOR_DATA in tran (SD spec,
 *     Physical Layer Simplified 4.10.1; stock never sends CMD13 on SD), inside the spec's write
 *     busy limits (4.6.2.2: 250 ms SDSC/SDHC, 500 ms SDXC).
 *   - errors: stock checks the response timeout, DCRC (bit 7) and EBE (bit 15), sends CMD12 after
 *     a multi-block error, resets only the FIFO and does not retry (SDCtrl.c:1110-1131); ours checks
 *     every error bit (0xAE80 / 0xAF82), always ends a CMD25 with CMD12, and tries twice more after
 *     a FIFO reset, as our reads do.
 *   - SDSC: byte addresses (lba << 9) after CMD16 512; SDHC/SDXC: block addresses (stock
 *     0x030605f0 tst #0x84; SD.c:601-604, SDM.c:601-604).
 *   - the card pulled: every polled wait gives up as soon as GPIO0.11 reads high (sd_out), and
 *     sd_read / sd_write answer SD_ERR_NOCARD without bus traffic (stock has no such handling: a
 *     pulled card runs into its 5 s waits).
 */


/* Card detect GPIO0.11, pulled up, 0 = card in (reference 0x03062c34). */
int sd_present(void)
{
    volatile uint32_t *gpio0 = (volatile uint32_t *)0x40160000u;

    if (!s_cd_ready) {
        GRF[1] = 0x00C00000u;                   /* GPIO0.11 -> GPIO */
        gpio0[1] &= ~(1u << 11);                /* input */
        GRF[5] = 0x00080008u;                   /* pull-up */
        delay_us(100);
        s_cd_ready = 1;
    }
    return !((gpio0[0x50 / 4] >> 11) & 1u);
}

static int sd_cmd(uint32_t idx, uint32_t arg, uint32_t flags)
{
    return send_cmd(idx, arg, flags, 0);
}

static int sd_acmd(uint32_t idx, uint32_t arg, uint32_t flags)
{
    if (sd_cmd(55, s_sd_rca << 16, F_RESP | F_CRC))
        return -1;
    return sd_cmd(idx, arg, flags);
}

static int sd_try_init(void)
{
    int v2 = 1;
    uint32_t i, r;

    s_sd_sectors = s_sd_rca = 0;
    s_sd_hc = s_sd_4bit = 0;
    if (!sd_present())
        return -1;
    select_mmc(1);
    clk_gate(0x18, 1);                          /* CLK_SDMMC, HCLK_SDMMC (SDK Hw_cru.h 24, 107) */
    clk_gate(0x6B, 1);
    clk_reset(0x11, 0);                         /* SDMMC_SRST 17 (SDK Hw_cru.h; stock SDA_Init 0x03060468
                                                 * port 0); RECHO 0x03062d84 used 0x12, eMMC's SDIO_SRST */
    CRU[0x30 / 4] = 0x009F009Fu;
    GRF[8] = 0xFC005400u;                       /* SD pinmux GPIO1.5..10 */
    GRF[9] = 0x003F0015u;
    delay_us(10);

    MMC[CTRL] = 7u;
    if (wait_clear(CTRL, 7u, 1000))
        return -20;
    MMC[FIFOTH] = 0x300F0010u;
    MMC[CTYPE] = 0;
    MMC[CLKSRC] = 0;
    MMC[TMOUT] = 0xFFFFFF40u;
    MMC[RINTSTS] = 0xFFFFFFFFu;
    MMC[INTMASK] = 0;
    MMC[PWREN] = 1;
    if (set_clock(400))
        return -2;
    delay_us(1000);

    sd_cmd(0, 0, F_INIT);
    delay_us(100);
    if (sd_cmd(8, 0x1AAu, F_RESP | F_CRC) == 0) {
        if ((MMC[RESP0] & 0xFFFu) != 0x1AAu)
            return -3;
    } else {
        v2 = 0;                                 /* SD 1.x */
    }
    for (i = 0; ; i++) {
        if (!sd_present())
            return -4;
        if (sd_cmd(55, 0, F_RESP | F_CRC))
            return -5;
        if (sd_cmd(41, v2 ? 0x40FF8000u : 0x00FF8000u, F_RESP) == 0 &&
            (MMC[RESP0] & 0x80000000u))
            break;
        if (i == 100u)
            return -6;
        delay_us(10000);
    }
    s_sd_hc = v2 && (MMC[RESP0] & 0x40000000u);
    if (sd_cmd(2, 0, F_RESP | F_LONG | F_CRC))
        return -7;
    for (i = 0; i < 3u && !s_sd_rca; i++) {
        if (sd_cmd(3, 0, F_RESP | F_CRC))
            return -8;
        s_sd_rca = MMC[RESP0] >> 16;
    }
    if (!s_sd_rca)
        return -9;
    if (sd_cmd(9, s_sd_rca << 16, F_RESP | F_LONG | F_CRC))
        return -10;
    if (MMC[RESP0 + 3] >> 30 == 0) {            /* CSD 1.0 (SDSC) */
        uint32_t c_size = ((MMC[RESP0 + 2] & 0x3FFu) << 2 | MMC[RESP0 + 1] >> 30) + 1u;
        uint32_t mult = (MMC[RESP0 + 1] >> 15) & 7u, bl = (MMC[RESP0 + 2] >> 16) & 15u;
        s_sd_sectors = (uint32_t)(((uint64_t)c_size << (mult + 2u) << bl) >> 9);
    } else {                                    /* CSD 2.0: (C_SIZE+1) * 512 KiB */
        r = ((MMC[RESP0 + 2] & 0x3Fu) << 16 | MMC[RESP0 + 1] >> 16) + 1u;
        s_sd_sectors = r >> 22 ? 0xFFFFFFFFu : r << 10;
    }
    if (s_sd_sectors == 0u)
        return -11;
    if (set_clock(24000))
        return -12;
    if (send_cmd(7, s_sd_rca << 16, F_RESP | F_CRC, 1))
        return -13;
    if (!s_sd_hc && sd_cmd(16, 512, F_RESP | F_CRC))
        return -14;
    sd_acmd(42, 0, F_RESP | F_CRC);             /* card-detect pull-up off */
    if (sd_acmd(6, 2, F_RESP | F_CRC) == 0) {   /* 4-bit bus */
        MMC[CTYPE] = 1;
        s_sd_4bit = 1;
    }
    clk_gate(0x6B, 0);
    return 0;
}

static void sd_off(void)
{
    select_mmc(1);
    clk_gate(0x6B, 1);
    MMC[CLKENA] = 0;
    update_clock();
    clk_gate(0x6B, 0);
    clk_gate(0x18, 0);
}

/* (A card in again, or out: whatever the old one had queued is dropped, counted; sdcard.c says so.) */
int sd_init(void)
{
    int r;

    wq_drop_sd();
    Q.st.sd_fail = 0;
    Q.st.dropped = 0;                    /* (the next card's count) */
    r = sd_try_init();

    if (r) {
        if (r != -1)
            sd_off();
        s_sd_sectors = 0;
    }
    return r;
}

uint32_t sd_sectors(void) { return s_sd_sectors; }
int sd_is_hc(void)        { return s_sd_hc; }
int sd_bus_width(void)    { return s_sd_4bit ? 4 : 1; }

/* The card's range check before a read or a write: 0 or -30 (also no card, or a failed one:
 * sd_fail made it 0 sectors). */
static int sd_check(uint32_t lba, uint32_t count)
{
    return lba + count < lba || lba + count > s_sd_sectors ? -30 : 0;
}


int sd_read(uint32_t lba, uint32_t count, void *buf)
{
    int r;

    if (count == 0u)
        return 0;
    if ((r = sd_check(lba, count)))
        return r;
    return q_read(1, lba, count, buf, (uint32_t)__builtin_return_address(0));
}

/* As sd_read; SDSC cards take byte addresses (CMD16 set 512-byte blocks at init). The queue first
 * (its blocks are older). A failed write is tried twice more after a FIFO reset, as reads are (the
 * same data again is harmless), unless the card is gone. The USB disk's and the big writes' path. */
int sd_write(uint32_t lba, uint32_t count, const void *buf)
{
    int r;

    watchdog_feed();
    if (count == 0u)
        return 0;
    emmc_sync();                             /* (first: a card that fails in it has 0 sectors then) */
    if ((r = sd_check(lba, count)))
        return r;
    select_mmc(1);
    for (int i = 0; ; i++) {
        if (sd_out())
            return SD_ERR_NOCARD;
        r = block_write_once(card_arg(1, lba), count, buf);
        if (!r || i == 2)
            break;
    }
    if (!r)
        s_sd_wr += count;
    return r && sd_out() ? SD_ERR_NOCARD : r;
}

/* The file system's writer on the card (services/sdcard.c): up to EMMC_WQ_SLOTS blocks through the
 * queue, bigger writes as one CMD25 after it; one of those failing fails the card as a queued
 * block does (nothing is written after a write that did not take). */
int sd_write_async(uint32_t lba, uint32_t count, const void *buf)
{
    int r = sd_check(lba, count);

    if (r || count <= EMMC_WQ_SLOTS)
        return r ? r : wq_put(1, lba, count, buf);
    if ((r = sd_write(lba, count, buf)))
        sd_fail(r);
    return r;
}


uint32_t sd_written(void) { return s_sd_wr; }

static uint32_t s_buf[256];

/* exFAT/FAT boot sector or an MBR with a first partition (reference
 * 0x03052a1c). */
static int is_volume_start(uint32_t lba)
{
    const uint8_t *b = (const uint8_t *)s_buf;

    if (lba >= s_sectors || emmc_read(lba, 1, s_buf) ||
        b[0x1FE] != 0x55 || b[0x1FF] != 0xAA)
        return 0;
    if (memcmp(b + 3, "EXFAT   ", 8) == 0 ||
        ((b[0] & 0xFD) == 0xE9 && (b[0x0B] | b[0x0C] << 8) == 0x200))
        return 1;
    return (b[0x1BE] & 0x7F) == 0 && b[0x1C2] != 0 &&
           (b[0x1C6] | b[0x1C7] | b[0x1C8] | b[0x1C9]) != 0;
}

/* IDB copies at LBA 0x40 + n*0x400: sector 0 magic (RC4'd 0x0FF0AA55),
 * "NANO" chip tag at +0x20A, system size in MiB at +0x202. The volume
 * normally starts right after the firmware area; otherwise the reference
 * scans forward in 1 MiB steps (this unit: 0x24000 holds a "SAVE" block,
 * the volume is at 0x88000). */
int emmc_layout(uint32_t *user_start)
{
    const uint8_t *b = (const uint8_t *)s_buf;

    for (uint32_t n = 0; n < 5u; n++) {
        uint32_t fw;

        if (emmc_read(0x40u + n * 0x400u, 2, s_buf) || s_buf[0] != 0xFCDC8C3Bu ||
            memcmp(b + 0x20A, "NANO", 4) != 0)
            continue;
        fw = (uint32_t)(b[0x202] | b[0x203] << 8) * 0x800u;
        if (fw == 0u || fw > s_sectors - 0x2000u)
            continue;
        for (uint32_t off = fw; off < 0x100000u; off += 0x800u) {
            if (is_volume_start(0x2000u + off)) {
                *user_start = s_write_floor = 0x2000u + off;
                s_fw_sectors = fw;
                return 0;
            }
        }
        return -2;
    }
    return -1;
}
