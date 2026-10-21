#ifndef TSDEMUX_H
#define TSDEMUX_H

#include <stddef.h>
#include <stdint.h>

typedef void (*ts_audio_fn)(const uint8_t *data, size_t len, void *user);

typedef struct {
    ts_audio_fn cb;
    void *user;
    uint8_t pkt[188];
    int fill;
    int pmt_pid;
    int aud_pid;
    int pat_done;
} ts_demux_t;

void ts_init(ts_demux_t *t, ts_audio_fn cb, void *user);
void ts_feed(ts_demux_t *t, const uint8_t *data, size_t len);

#endif
