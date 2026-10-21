#ifndef JOBS_H
#define JOBS_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define SEARCH_MAX 40

void jobs_init(void);
void jobs_shutdown(void);

void jobs_search(const char *query);
int jobs_search_busy(void);
int jobs_search_count(void);
int jobs_search_get(int i, track_t *t);
void jobs_search_error(char *out, size_t cap);
void jobs_search_query(char *out, size_t cap);

void jobs_radio(const char *video_id);

void jobs_art(const char *url, const char *id);
int jobs_art_take(char *id, uint8_t **data, size_t *len);

#endif
