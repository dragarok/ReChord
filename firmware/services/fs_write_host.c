/* Host builds (tools/test_fs_*.py): fs_write_ext.c linked in directly, no overlay. On the device
 * fsx_api() is startup/ap_ovl.c's gate (ovl_get(OVL_FSW)). FSX_OFF=1 in the environment: as with
 * the overlay refused. */
#include <stdlib.h>

#include "fs_target_int.h"

int fsx_create(vol_t *v, const char *leaf, size_t n, uint8_t attr, uint32_t clus);
int fsx_grow(struct fs_file *f);
int fsx_close(struct fs_file *f);
int fsx_open(struct fs_file *f, int append);
int fsx_path(int op, const char *a, const char *b, fs_stat_t *st);
int fsx_clean(vol_t *v);

static const fsx_api_t k_fsx = { fsx_create, fsx_grow, fsx_close, fsx_open, fsx_path, fsx_clean };

const fsx_api_t *fsx_api(void)
{
    return getenv("FSX_OFF") ? 0 : &k_fsx;   /* FSX_OFF=1: as with the overlay refused */
}
