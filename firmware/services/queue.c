/*
 * queue.c -- the play queue (queue.h). The folder listings come from dirlist.c
 * (the browser's view is reloaded by the browser afterwards).
 */
#include <string.h>

#include "queue.h"
#include "dirlist.h"
#include "drivers/clock.h"

typedef struct {
    fs_ref_t ref;
    uint8_t kind;
} q_ent_t;

static q_ent_t s_q[Q_MAX];
static uint16_t s_order[Q_MAX];              /* play order -> queue entry */
static int s_n, s_mode, s_stream, s_pos;
static uint32_t s_rng = 0x9E3779B9u;

static void add_file(int i, int *incomplete)
{
    if (s_n == Q_MAX) {
        *incomplete = 1;
        return;
    }
    s_q[s_n].ref = *dirlist_ref(i);
    s_q[s_n].kind = (uint8_t)dirlist_kind_at(i);
    s_order[s_n] = (uint16_t)s_n;
    s_n++;
}

void queue_clear(void)
{
    s_n = s_stream = s_pos = 0;
}

int queue_from_folder(const char *path, const fs_ref_t *folder, const fs_ref_t *start, int *incomplete)
{
    int page = 0, more = 1, first = 0;

    queue_clear();
    *incomplete = 0;
    while (more && s_n < Q_MAX) {
        int n = dirlist_load(path, folder, page++, &more);

        if (n < 0)
            return n;
        for (int i = 0; i < n; i++) {
            if (dirlist_is_dir(i))
                continue;
            if (start && !memcmp(dirlist_ref(i), start, sizeof *start))
                first = s_n;
            add_file(i, incomplete);
        }
    }
    if (more)
        *incomplete = 1;
    s_stream = s_pos = first;
    if (s_mode == Q_SHUFFLE)
        queue_set_mode(Q_SHUFFLE);
    return 0;
}

int queue_from_list(int n, int start, int (*get)(int i, fs_ref_t *r, int *kind))
{
    int first = 0;

    queue_clear();
    for (int i = 0; i < n && s_n < Q_MAX; i++) {
        int kind;

        if (get(i, &s_q[s_n].ref, &kind))
            continue;
        if (i <= start)
            first = s_n;                     /* (the start, or the last track before it) */
        s_q[s_n].kind = (uint8_t)kind;
        s_order[s_n] = (uint16_t)s_n;
        s_n++;
    }
    s_stream = s_pos = first;
    if (s_mode == Q_SHUFFLE)
        queue_set_mode(Q_SHUFFLE);
    return s_n ? 0 : FS_ERR_OPEN;
}

/* Depth first, sorted: a folder's files, then each of its subfolders. Pending
 * folders wait on a stack (pushed in reverse so they come off in order). */
#define PENDING 128

int queue_from_tree(const char *path, const fs_ref_t *folder, int *incomplete)
{
    static struct { fs_ref_t ref; uint8_t depth; } stack[PENDING];
    int sp = 0, first = 1;
    fs_ref_t cur;
    uint8_t depth = 0;

    queue_clear();
    *incomplete = 0;
    for (;;) {
        int page = 0, more = 1;

        while (more) {
            int n = dirlist_load(first ? path : NULL, first ? folder : &cur, page++, &more), dirs = 0;

            if (n < 0)
                return first ? n : 0;
            for (int i = 0; i < n; i++)
                if (!dirlist_is_dir(i))
                    add_file(i, incomplete);
                else
                    dirs++;
            if (dirs && depth >= Q_DEPTH)
                *incomplete = 1;                 /* deeper folders are not taken */
            else
                for (int i = n - 1; i >= 0; i--)
                    if (dirlist_is_dir(i)) {
                        if (sp == PENDING) {
                            *incomplete = 1;
                            break;
                        }
                        stack[sp].ref = *dirlist_ref(i);
                        stack[sp++].depth = (uint8_t)(depth + 1u);
                    }
        }
        first = 0;
        if (!sp || s_n == Q_MAX)
            break;
        sp--;
        cur = stack[sp].ref;
        depth = stack[sp].depth;
    }
    if (sp)
        *incomplete = 1;
    s_stream = s_pos = 0;
    if (s_mode == Q_SHUFFLE)
        queue_set_mode(Q_SHUFFLE);
    return 0;
}

int queue_count(void) { return s_n; }
int queue_mode(void) { return s_mode; }

const char *queue_mode_name(int mode)
{
    static const char *const k[Q_MODES] = { "normal", "shuffle", "repeat track", "repeat all" };

    return mode >= 0 && mode < Q_MODES ? k[mode] : "?";
}

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

void queue_set_mode(int mode)
{
    int cur = s_n ? s_order[s_pos] : 0;

    if (mode < 0 || mode >= Q_MODES)
        return;
    s_mode = mode;
    for (int i = 0; i < s_n; i++)
        s_order[i] = (uint16_t)i;
    if (mode == Q_SHUFFLE && s_n > 1) {
        s_rng ^= time_us();
        s_order[0] = (uint16_t)cur;             /* the current track stays first */
        s_order[cur] = 0;
        for (int i = s_n - 1; i > 1; i--) {     /* Fisher-Yates over the rest */
            int j = 1 + (int)(rnd() % (uint32_t)i);
            uint16_t t = s_order[i];

            s_order[i] = s_order[j];
            s_order[j] = t;
        }
        s_pos = 0;
    } else {
        s_pos = cur;
    }
    s_stream = s_pos + 1;                       /* the engine has the current one already */
}

int queue_take(fs_ref_t *ref, int *kind, int *pos)
{
    int p = s_stream;

    if (!s_n)
        return -1;
    if (s_mode == Q_REPEAT_ONE)
        p = s_pos;                              /* the same track again, gapless */
    else if (p >= s_n) {
        if (s_mode != Q_REPEAT_ALL)
            return -1;
        p = 0;
    }
    *ref = s_q[s_order[p]].ref;
    *kind = s_q[s_order[p]].kind;
    *pos = p;
    s_stream = p + 1;
    return 0;
}

void queue_seek(int pos)
{
    if (pos >= 0 && pos < s_n)
        s_stream = s_pos = pos;
}

void queue_heard(int pos)
{
    if (pos >= 0 && pos < s_n)
        s_pos = pos;
}

int queue_pos(void) { return s_pos; }

int queue_next_pos(void)
{
    if (s_pos + 1 < s_n)
        return s_pos + 1;
    return s_mode == Q_REPEAT_ALL && s_n ? 0 : -1;
}

int queue_prev_pos(void)
{
    if (s_pos > 0)
        return s_pos - 1;
    return s_mode == Q_REPEAT_ALL && s_n ? s_n - 1 : -1;
}

const fs_ref_t *queue_ref(int pos)
{
    return pos >= 0 && pos < s_n ? &s_q[s_order[pos]].ref : NULL;
}
