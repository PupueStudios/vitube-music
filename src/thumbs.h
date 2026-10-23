#ifndef THUMBS_H
#define THUMBS_H

#include <vita2d.h>

#include "util.h"

void thumbs_init(void);
void thumbs_shutdown(void);
void thumbs_pump(void);
vita2d_texture *thumbs_get(const track_t *t, float *fade);

#endif
