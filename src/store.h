#ifndef STORE_H
#define STORE_H

#include <stddef.h>

#include "util.h"

#define DL_DIR DATA_DIR "/downloads"

void store_init(void);
int store_fav_count(void);
int store_fav_get(int i, track_t *t);
int store_is_fav(const char *id);
int store_fav_toggle(const track_t *t);
int store_hist_count(void);
int store_hist_get(int i, track_t *t);
void store_hist_push(const track_t *t);

int store_pl_count(void);
int store_pl_name(int pl, char *out, size_t cap);
int store_pl_track_count(int pl);
int store_pl_track_get(int pl, int i, track_t *t);
int store_pl_create(const char *name);
int store_pl_rename(int pl, const char *name);
int store_pl_delete(int pl);
int store_pl_add(int pl, const track_t *t);
int store_pl_remove_track(int pl, int i);

int store_dl_count(void);
int store_dl_get(int i, track_t *t);
int store_dl_find(const char *id, int *frames, int *dur_ms);
void store_dl_add(const track_t *t, int frames, int dur_ms);
void store_dl_remove(const char *id);
void store_dl_path(const char *id, const char *ext, char *out, size_t cap);

#endif
