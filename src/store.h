#ifndef STORE_H
#define STORE_H

#include "util.h"

void store_init(void);
int store_fav_count(void);
int store_fav_get(int i, track_t *t);
int store_is_fav(const char *id);
int store_fav_toggle(const track_t *t);
int store_hist_count(void);
int store_hist_get(int i, track_t *t);
void store_hist_push(const track_t *t);

#endif
