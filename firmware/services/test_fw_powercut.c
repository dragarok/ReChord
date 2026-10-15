/*
 * test_fw_powercut.c -- fw_update_install() and fw_update_repair() under a power cut at every
 * point. An in-memory disk: two firmware slots holding an OLD image, a NEW image elsewhere.
 * For every cut c (0 .. all the sectors the install writes), the install runs with a budget of
 * c sectors -- a transfer the cut lands in writes only its first sectors -- and then stops.
 * Then the boot as the research found it (docs/USB_COMPLIANCE.md, ROM report): a slot is valid
 * if "RKnanoFW" is at 0x1F8 of its sector 0 and the copy of sector 0 at fw_end matches; FW1 if
 * valid, else FW2 (at half the firmware area); both valid: the newer date. Required at every c:
 *   - something boots, and what boots is OLD or NEW complete (every sector 0..fw_end);
 *   - after fw_update_repair() on that state (no cut), both slots are valid and complete.
 *   test_fw_powercut   -> "N cut points ok" or the first failure; exit 0/1
 *   FWPC_BOOT_ONLY=1   -> only the boot check (for an updater without fw_update_repair)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/emmc.h"
#include "services/fw_update.h"

#define FW_SECTORS 2048u              /* two 512-sector slots */
#define NEW_LBA    (EMMC_FW_BASE + FW_SECTORS + 64u)
#define DISK       (NEW_LBA + 1024u)
#define N_OLD      37u                /* fw_end, sectors (not a multiple of the 16-sector block) */
#define N_NEW      51u

static uint8_t s_disk[DISK * 512];
static long s_budget = -1;            /* sectors the "power" still allows; -1 = no cut */
static long s_written;

int emmc_read(uint32_t lba, uint32_t count, void *buf)
{
    if (lba + count > DISK)
        return -30;
    memcpy(buf, s_disk + (size_t)lba * 512, (size_t)count * 512);
    return 0;
}

int emmc_write(uint32_t lba, uint32_t count, const void *buf) { (void)lba; (void)count; (void)buf; return -30; }

uint32_t emmc_fw_sectors(void) { return FW_SECTORS; }

int emmc_fw_write(uint32_t rel, uint32_t count, const void *buf)
{
    for (uint32_t i = 0; i < count; i++) {           /* sector by sector: a cut can land mid-transfer */
        if (s_budget == 0)
            return -99;
        if (s_budget > 0)
            s_budget--;
        memcpy(s_disk + (size_t)(EMMC_FW_BASE + rel + i) * 512, (const uint8_t *)buf + (size_t)i * 512, 512);
        s_written++;
    }
    return 0;
}

static void image(uint8_t *p, uint32_t n, uint8_t seed, uint16_t year)
{
    for (uint32_t i = 0; i < (n + 1u) * 512u; i++)
        p[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 9));
    p[0] = (uint8_t)year; p[1] = (uint8_t)(year >> 8);
    memcpy(p + 0x1F8, "RKnanoFW", 8);
    p[0x1F4] = (uint8_t)(n * 512u); p[0x1F5] = (uint8_t)((n * 512u) >> 8);
    p[0x1F6] = (uint8_t)((n * 512u) >> 16); p[0x1F7] = 0;
    memcpy(p + (size_t)n * 512, p, 512);             /* the header copy at fw_end */
}

static uint8_t s_old[(N_OLD + 1) * 512], s_new[(N_NEW + 1) * 512];

/* 0 invalid; else fw_end's sector */
static uint32_t valid(uint32_t rel)
{
    const uint8_t *h = s_disk + (size_t)(EMMC_FW_BASE + rel) * 512;
    uint32_t end;

    if (memcmp(h + 0x1F8, "RKnanoFW", 8))
        return 0;
    end = (h[0x1F4] | h[0x1F5] << 8 | (uint32_t)h[0x1F6] << 16) >> 9;
    if (!end || end >= FW_SECTORS / 2u)
        return 0;
    return memcmp(h, s_disk + (size_t)(EMMC_FW_BASE + rel + end) * 512, 512) ? 0 : end;
}

/* what the slot holds: 1 OLD complete, 2 NEW complete, 0 neither */
static int holds(uint32_t rel)
{
    const uint8_t *s = s_disk + (size_t)(EMMC_FW_BASE + rel) * 512;

    if (!memcmp(s, s_old, sizeof s_old))
        return 1;
    if (!memcmp(s, s_new, sizeof s_new))
        return 2;
    return 0;
}

static int boots(void)
{
    uint32_t s2 = FW_SECTORS / 2u, v1 = valid(0), v2 = valid(s2);
    uint32_t pick;

    if (!v1 && !v2)
        return -1;
    pick = !v2 ? 0 : !v1 ? s2 : 0;
    if (v1 && v2) {                                  /* the newer date (u16 year here) */
        const uint8_t *a = s_disk + (size_t)EMMC_FW_BASE * 512, *b = s_disk + (size_t)(EMMC_FW_BASE + s2) * 512;
        pick = (b[0] | b[1] << 8) > (a[0] | a[1] << 8) ? s2 : 0;
    }
    return holds(pick) ? (int)pick : -2;
}

int main(int argc, char **argv)
{
    static fw_image_t img;
    long total, c;
    int bad = 0;

    (void)argc; (void)argv;
    image(s_old, N_OLD, 1, 2025);
    image(s_new, N_NEW, 2, 2026);
    memset(&img, 0, sizeof img);
    img.lba = NEW_LBA;
    img.fw_end = N_NEW * 512u;
    /* how much a full install writes */
    memset(s_disk, 0, sizeof s_disk);
    memcpy(s_disk + (size_t)EMMC_FW_BASE * 512, s_old, sizeof s_old);
    memcpy(s_disk + (size_t)(EMMC_FW_BASE + FW_SECTORS / 2u) * 512, s_old, sizeof s_old);
    memcpy(s_disk + (size_t)NEW_LBA * 512, s_new, sizeof s_new);
    s_budget = -1;
    s_written = 0;
    if (fw_update_install(&img, NULL) || holds(0) != 2 || holds(FW_SECTORS / 2u) != 2) {
        printf("FAIL: the uncut install does not leave NEW in both slots\n");
        return 1;
    }
    total = s_written;
    for (c = 0; c <= total; c++) {
        int b, r;

        memset(s_disk, 0, sizeof s_disk);
        memcpy(s_disk + (size_t)EMMC_FW_BASE * 512, s_old, sizeof s_old);
        memcpy(s_disk + (size_t)(EMMC_FW_BASE + FW_SECTORS / 2u) * 512, s_old, sizeof s_old);
        memcpy(s_disk + (size_t)NEW_LBA * 512, s_new, sizeof s_new);
        s_budget = c;
        (void)fw_update_install(&img, NULL);
        s_budget = -1;
        b = boots();
        if (b < 0) {
            printf("FAIL: cut after %ld of %ld sectors: %s\n", c, total,
                   b == -1 ? "no slot is valid (brick)" : "the slot that boots is incomplete (brick)");
            bad = 1;
            break;
        }
        if (getenv("FWPC_BOOT_ONLY"))                /* (judging an updater with no repair) */
            continue;
        r = fw_update_repair(NULL);                  /* the next boot heals the other slot */
        if (r < 0 || !valid(0) || !valid(FW_SECTORS / 2u) || !holds(0) || !holds(FW_SECTORS / 2u)) {
            printf("FAIL: cut after %ld of %ld sectors: repair %d left slots %d/%d\n", c, total, r, holds(0),
                   holds(FW_SECTORS / 2u));
            bad = 1;
            break;
        }
    }
    if (!bad)
        printf("%ld cut points ok: something complete always boots; repair always restores both slots\n", total + 1);
    return bad;
}
