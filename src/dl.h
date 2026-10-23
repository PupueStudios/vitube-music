#ifndef DL_H
#define DL_H

#include <stddef.h>

#include "util.h"

typedef struct {
    int active;
    int pct;
    int pending;
    char title[128];
} dl_status_t;

void dl_init(void);
void dl_shutdown(void);
int dl_enqueue(const track_t *t);
void dl_status(dl_status_t *s);
int dl_take_event(char *out, size_t cap);

#endif
