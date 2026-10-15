/* Core B: ASF data packets -> media objects (b_asf.h). */
#include "b_asf.h"

int b_asf_open(b_asf_t *a)
{
    uint32_t len;

    if (b_in_read(&len, 4) != 4 || len != sizeof a->cfg || b_in_read(&a->cfg, len) != len)
        return -1;
    if (a->cfg.channels != b_trk.channels || a->cfg.rate != b_trk.rate ||
        a->cfg.packet_size < 16 || a->cfg.packet_size > 0x10000u)
        return -2;
    a->pkt = b_alloc(a->cfg.packet_size + B_ASF_PAD);
    if (!a->pkt)
        return -1;
    memset(a->pkt + a->cfg.packet_size, 0, B_ASF_PAD);
    b_in_need = a->cfg.packet_size;          /* one packet per step */
    return 0;
}

int b_asf_object(b_asf_t *a, const uint8_t **data, uint32_t *len)
{
    asf_payload_t pl;

    for (;;) {
        int r;

        if (!a->in_pkt) {
            if (b_in_read(a->pkt, a->cfg.packet_size) != a->cfg.packet_size)
                return 0;
            if (asf_pkt_begin(&a->it, a->pkt, a->cfg.packet_size) != 0) {
                a->errors++;
                continue;
            }
            a->in_pkt = 1;
        }
        r = asf_pkt_next(&a->it, &pl);
        if (r <= 0) {
            if (r < 0)
                a->errors++;
            a->in_pkt = 0;
            continue;
        }
        if (pl.stream != a->cfg.stream || !pl.len)
            continue;
        if (pl.obj_off == 0 && pl.len >= pl.obj_size) {
            a->obj_on = 0;                   /* whole: use it where it is */
            *data = pl.data;
            *len = pl.len;
            return 1;
        }
        if (pl.obj_off == 0) {               /* first fragment */
            if (pl.obj_size > a->obj_cap) {
                b_free(a->obj);
                a->obj = b_alloc(pl.obj_size + B_ASF_PAD);
                a->obj_cap = a->obj ? pl.obj_size : 0;
            }
            a->obj_on = a->obj != NULL;
            a->obj_num = pl.obj;
            a->obj_size = pl.obj_size;
            a->obj_have = 0;
        }
        if (!a->obj_on || pl.obj != a->obj_num || pl.obj_off != a->obj_have ||
            pl.len > a->obj_size - a->obj_have) {
            if (a->obj_on)
                a->errors++;                 /* a fragment went missing: drop the object */
            a->obj_on = 0;
            continue;
        }
        memcpy(a->obj + a->obj_have, pl.data, pl.len);
        a->obj_have += pl.len;
        if (a->obj_have == a->obj_size) {
            memset(a->obj + a->obj_size, 0, B_ASF_PAD);
            a->obj_on = 0;
            *data = a->obj;
            *len = a->obj_size;
            return 1;
        }
    }
}
