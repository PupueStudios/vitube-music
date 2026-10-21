#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

static mutex_t g_log_mx;
static FILE *g_log;

void buf_init(buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int buf_append(buf_t *b, const void *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *nd = realloc(b->data, nc);
        if (!nd) return -1;
        b->data = nd;
        b->cap = nc;
    }
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

void buf_free(buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void mx_init(mutex_t *m, const char *name) {
    m->id = sceKernelCreateMutex(name, 2, 0, NULL);
}

void mx_lock(mutex_t *m) {
    sceKernelLockMutex(m->id, 1, NULL);
}

void mx_unlock(mutex_t *m) {
    sceKernelUnlockMutex(m->id, 1);
}

void sleep_ms(int ms) {
    sceKernelDelayThread((SceUInt)ms * 1000u);
}

uint32_t now_ms(void) {
    return (uint32_t)(sceKernelGetProcessTimeWide() / 1000ull);
}

void log_init(void) {
    mx_init(&g_log_mx, "ytm_log");
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
    const char *path = DATA_DIR "/log.txt";
    g_log = fopen(path, "wb");
    log_msg("---- start ----");
}

void log_msg(const char *fmt, ...) {
    if (!g_log) return;
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    mx_lock(&g_log_mx);
    fprintf(g_log, "[%u] %s\n", (unsigned)now_ms(), line);
    fflush(g_log);
    mx_unlock(&g_log_mx);
}

void str_set(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int utf8_to_utf16(const char *s, uint16_t *out, int cap) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < cap - 1) {
        uint32_t c = *p;
        int extra = 0;
        if (c < 0x80) {
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            c &= 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            c &= 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            c &= 0x07;
            extra = 3;
        } else {
            p++;
            continue;
        }
        p++;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++, p++) c = (c << 6) | (*p & 0x3F);
        if (c >= 0x10000) {
            if (n + 2 >= cap) break;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (c >> 10));
            out[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            out[n++] = (uint16_t)c;
        }
    }
    out[n] = 0;
    return n;
}

void utf16_to_utf8(const uint16_t *s, char *out, size_t cap) {
    size_t n = 0;
    while (*s && n + 5 < cap) {
        uint32_t c = *s++;
        if (c >= 0xD800 && c < 0xDC00 && *s >= 0xDC00 && *s < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (*s - 0xDC00);
            s++;
        }
        if (c < 0x80) {
            out[n++] = (char)c;
        } else if (c < 0x800) {
            out[n++] = (char)(0xC0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[n++] = (char)(0xF0 | (c >> 18));
            out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = '\0';
}

int parse_duration(const char *s) {
    if (!s || !*s) return -1;
    int parts[3] = {0, 0, 0};
    int np = 0;
    int digits = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            if (np >= 3) return -1;
            parts[np] = parts[np] * 10 + (*p - '0');
            digits++;
        } else if (*p == ':' || *p == '\0') {
            if (!digits) return -1;
            np++;
            digits = 0;
            if (*p == '\0') break;
        } else {
            return -1;
        }
    }
    if (np < 2) return -1;
    if (np == 2) return parts[0] * 60 + parts[1];
    return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

int starts_with(const char *s, const char *prefix) {
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}
