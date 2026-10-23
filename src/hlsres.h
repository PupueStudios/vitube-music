#ifndef HLSRES_H
#define HLSRES_H

#include <stddef.h>

#include "hls.h"
#include "net.h"

int hlsres_resolve(http_t *h, const char *video_id, hls_media_t *out, volatile int *cancel, char *err, size_t ec);

#endif
