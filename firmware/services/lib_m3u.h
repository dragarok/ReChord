/*
 * lib_m3u.h -- favourites and playlists (services/lib_m3u.c): M3U8 files keyed by path.
 */
#ifndef RECHORD_LIB_M3U_H
#define RECHORD_LIB_M3U_H

#include <stdint.h>

#define M3U_LISTS  20                /* playlists listed (the favourites first) */
#define M3U_EVERY  16                /* an offset kept every 16 entries of the open list */
#define M3U_MAX    2048              /* entries a list can hold (more: not shown, not added to) */
#define M3U_SPARSE (M3U_MAX / M3U_EVERY)
#define M3U_LINE   300
#define M3U_PATH   280

typedef struct {
    char name[32];                   /* what the rows show: #PLAYLIST, else from the file name */
    char file[32];                   /* in the lists folder (longer names: not listed) */
    uint32_t size;
    uint16_t n;                      /* entries (once opened) */
    uint8_t fav;
} lib_list_t;

typedef struct {
    char path[M3U_PATH];             /* the device's: "/SD/Music/..." */
    char title[120], artist[80];
    int dur;                         /* seconds, -1 unknown */
} m3u_ent_t;

extern lib_list_t g_lists[M3U_LISTS];
extern int g_nlists;

int m3u_lists(void);                 /* g_lists again: [0] the favourites; the count */
int m3u_open(int k);                 /* list k's entries indexed: their count, or < 0 */
int m3u_count(void);
int m3u_entry(int i, m3u_ent_t *e);  /* entry i of the open list */
int m3u_has(int k, const char *path);
int m3u_append(int k, const m3u_ent_t *a, int n);
int m3u_remove(int k, int i);
int m3u_move(int k, int i, int dir); /* -1 up, +1 down */
int m3u_rename(int k, const char *name);
int m3u_delete(int k);
int m3u_create(const char *name);    /* its index, or < 0 */
int m3u_move_all(const char *from);  /* lists in folder `from` merged into the lists folder */
void m3u_path(int k, char *out, int n);

#endif
