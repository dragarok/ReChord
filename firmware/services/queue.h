/*
 * queue.h -- what plays next: up to Q_MAX tracks as file refs (fs.h), from one
 * folder (sorted, starting at the chosen file) or from a folder and everything
 * under it (each folder's files, then its subfolders, depth first, sorted).
 * Play modes: in order, shuffle, repeat the track, repeat all.
 *
 * Two positions: the track being heard (the UI's), and the next one to hand to
 * the audio engine, which runs a few tracks ahead for gapless playback.
 */
#ifndef RECHORD_QUEUE_H
#define RECHORD_QUEUE_H

#include <stdint.h>

#include "fs.h"

#define Q_MAX 400
#define Q_DEPTH 8                            /* folder levels below the root of a tree */

enum { Q_NORMAL = 0, Q_SHUFFLE, Q_REPEAT_ONE, Q_REPEAT_ALL, Q_MODES };

/* 0, or FS_ERR_*. *incomplete: the folder held more than the queue takes. */
int queue_from_folder(const char *path, const fs_ref_t *folder, const fs_ref_t *start, int *incomplete);
int queue_from_tree(const char *path, const fs_ref_t *folder, int *incomplete);
void queue_clear(void);

int queue_count(void);
int queue_mode(void);
void queue_set_mode(int mode);               /* shuffle: a new order with the current track first */
const char *queue_mode_name(int mode);

/* The next track for the audio engine: 0, or -1 at the end of the queue. *pos is
 * its place in play order (what queue_heard/next/prev take). */
int queue_take(fs_ref_t *ref, int *kind, int *pos);
/* Restart the hand-out at play-order position pos. */
void queue_seek(int pos);
/* The audio engine says which position is being heard now. */
void queue_heard(int pos);
int queue_pos(void);
/* Positions for the next / previous track buttons (repeat-all wraps); -1 = none. */
int queue_next_pos(void);
int queue_prev_pos(void);
/* Track at a play-order position. */
const fs_ref_t *queue_ref(int pos);

#endif
