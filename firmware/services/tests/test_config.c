/*
 * test_config.c — host tests for RECHORD.CFG (services/config.c).
 *
 * Covers the device's finding of 2026-10-18: a line added on the PC during a USB disk session
 * ("hifi=verify") was overwritten by the player's copy at the next save. After a session,
 * config_reload_if_changed() must read an edited file again (and leave an unedited one alone),
 * and the save that follows must keep the PC's edit.
 */
#include "config.h"
#include "fs.h"
#include "fs_host.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int s_checks;
static int s_failures;

#define CHECK(cond) do {                                                   \
    s_checks++;                                                            \
    if (!(cond)) {                                                         \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        s_failures++;                                                      \
    }                                                                      \
} while (0)

/* config.c's other dependencies, stubbed */
uint32_t time_ms(void) { return 1000u; }
const char *theme_name(int i) { return i == 0 ? "Classic" : NULL; }
int theme_index(const char *name) { return strcmp(name, "Classic") ? -1 : 0; }

#define FILE_ON_HOST ".work/Internal/RECHORD.CFG"

static void file_put(const char *text)
{
    FILE *fp = fopen(FILE_ON_HOST, "wb");

    CHECK(fp != NULL);
    if (fp) {
        fputs(text, fp);
        fclose(fp);
    }
}

static void file_append(const char *text)
{
    FILE *fp = fopen(FILE_ON_HOST, "ab");

    CHECK(fp != NULL);
    if (fp) {
        fputs(text, fp);
        fclose(fp);
    }
}

static int file_has(const char *line)
{
    char buf[2048];
    FILE *fp = fopen(FILE_ON_HOST, "rb");
    size_t n;

    if (!fp)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[n] = 0;
    return strstr(buf, line) != NULL;
}

static void test_pc_edit_survives(void)
{
    file_put("play=shuffle\nvolume=-20\n");
    CHECK(config_load() == 0);
    CHECK(g_cfg.hifi == 0);
    CHECK(config_save() == 0);                    /* the player's own text: the file is ours now */
    CHECK(config_reload_if_changed() == 0);       /* nothing changed on the PC */

    file_append("hifi=verify\n");                 /* the PC's edit during the disk session */
    CHECK(config_reload_if_changed() == 1);
    CHECK(g_cfg.hifi == 2);                       /* (k_hifi: auto, off, verify) */
    CHECK(config_save() == 0);                    /* the save at power-off */
    CHECK(file_has("hifi=verify\n"));             /* still there (the device lost it) */
    CHECK(config_reload_if_changed() == 0);       /* our save is what the file holds */
}

static void test_pc_changed_a_value(void)
{
    file_put("play=shuffle\nvolume=-20\n");
    CHECK(config_load() == 0);
    CHECK(config_save() == 0);
    file_put("play=normal\nvolume=-30\n");
    CHECK(config_reload_if_changed() == 1);
    CHECK(g_cfg.volume == -30);
    CHECK(config_save() == 0);
    CHECK(file_has("volume=-30\n"));
    CHECK(file_has("play=normal\n"));
}

static void test_pc_deleted_the_file(void)
{
    file_put("play=shuffle\nvolume=-20\n");
    CHECK(config_load() == 0);
    CHECK(config_save() == 0);
    remove(FILE_ON_HOST);
    CHECK(config_reload_if_changed() == 1);       /* the defaults, as at a boot without the file */
}

int main(void)
{
    mkdir(".work", 0755);
    mkdir(".work/Internal", 0755);
    fs_host_set_root(".work");

    test_pc_edit_survives();
    test_pc_changed_a_value();
    test_pc_deleted_the_file();

    printf("test_config: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
