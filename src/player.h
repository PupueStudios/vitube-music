#ifndef PLAYER_H
#define PLAYER_H

#include <stddef.h>

#include "util.h"

typedef enum {
    PL_IDLE = 0,
    PL_LOADING,
    PL_PLAYING,
    PL_PAUSED,
    PL_ERROR
} pl_state_t;

int player_init(void);
void player_shutdown(void);

void player_queue_set(const track_t *t, int n, int start);
int player_queue_add(const track_t *t);
void player_queue_append(const track_t *t, int n);
int player_queue_count(void);
int player_queue_cur_index(void);
int player_queue_remove(int idx);
int player_queue_trim(void);
int player_queue_get(int i, track_t *t);
int player_current(track_t *t);

void player_play_index(int idx);
void player_next(void);
void player_prev(void);
void player_toggle_pause(void);
void player_seek_rel(int ms);
void player_seek_abs(int ms);

pl_state_t player_state(void);
int player_position_ms(void);
int player_duration_ms(void);
void player_message(char *out, size_t cap);
void player_set_radio(int on);

#endif
