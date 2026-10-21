#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

#define DATA_DIR "ux0:data/vitube-music"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf_t;

typedef struct {
    int id;
} mutex_t;

typedef struct {
    char id[16];
    char title[128];
    char artist[96];
    char album[96];
    char thumb[256];
    int duration;
} track_t;

void buf_init(buf_t *b);
int buf_append(buf_t *b, const void *p, size_t n);
void buf_free(buf_t *b);

void mx_init(mutex_t *m, const char *name);
void mx_lock(mutex_t *m);
void mx_unlock(mutex_t *m);

void sleep_ms(int ms);
uint32_t now_ms(void);

void log_init(void);
void log_msg(const char *fmt, ...);

void str_set(char *dst, size_t cap, const char *src);
int utf8_to_utf16(const char *s, uint16_t *out, int cap);
void utf16_to_utf8(const uint16_t *s, char *out, size_t cap);
int parse_duration(const char *s);
int starts_with(const char *s, const char *prefix);

#endif
