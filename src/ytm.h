#ifndef YTM_H
#define YTM_H

#include <stddef.h>

#include "net.h"
#include "util.h"

#define YTM_UA_VISION \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15"

void ytm_configure(const char *hl, const char *gl);

int ytm_search(http_t *h, const char *query, track_t *out, int max, volatile int *cancel, char *err, size_t errcap);
int ytm_radio(http_t *h, const char *video_id, track_t *out, int max, volatile int *cancel, char *err, size_t errcap);
int ytm_hls_master(http_t *h, const char *video_id, char *url, size_t cap, volatile int *cancel, char *err, size_t errcap);

#endif
