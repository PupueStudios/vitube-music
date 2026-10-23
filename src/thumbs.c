#include "thumbs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "net.h"
#include "store.h"

#define CACHE 40
#define MIN_FREE 4
#define MAX_BYTES (256 * 1024)
#define FETCH_THREADS 2

enum {
    T_FREE = 0,
    T_QUEUED,
    T_FETCHING,
    T_FETCHED,
    T_DECODING,
    T_READY,
    T_FAILED
};

typedef struct {
    int state;
    char id[16];
    char url[300];
    uint32_t used;
    uint32_t at;
    vita2d_texture *tex;
    uint8_t *data;
    size_t len;
} entry_t;

static struct {
    mutex_t mx;
    entry_t e[CACHE];
    volatile int quit;
} T;

static volatile int g_never_cancel;
static volatile int g_fail_logs;
static SceUID g_th[FETCH_THREADS];

static int find_locked(const char *id) {
    for (int i = 0; i < CACHE; i++)
        if (T.e[i].state != T_FREE && strcmp(T.e[i].id, id) == 0) return i;
    return -1;
}

static int free_slot_locked(void) {
    for (int i = 0; i < CACHE; i++)
        if (T.e[i].state == T_FREE) return i;
    return -1;
}

static void build_url(const track_t *t, char *out, size_t cap) {
    char local[96];
    store_dl_path(t->id, "jpg", local, sizeof local);
    FILE *f = fopen(local, "rb");
    if (f) {
        fclose(f);
        str_set(out, cap, local);
        return;
    }
    if (t->thumb[0] && !strstr(t->thumb, "i.ytimg.com/vi/")) {
        str_set(out, cap, t->thumb);
        if (strstr(out, "googleusercontent.com") || strstr(out, "ggpht.com")) {
            char *eq = strrchr(out, '=');
            if (eq && (size_t)(eq - out) + 24 < cap) strcpy(eq, "=w128-h128-l90-rj");
        }
        return;
    }
    snprintf(out, cap, "https://i.ytimg.com/vi/%s/mqdefault.jpg", t->id);
}

static int fetch_url(http_t *h, const char *url, buf_t *b) {
    if (starts_with(url, "ux0:")) {
        FILE *lf = fopen(url, "rb");
        if (!lf) return 0;
        uint8_t tmp[4096];
        size_t n;
        int ok = 1;
        while ((n = fread(tmp, 1, sizeof tmp, lf)) > 0) {
            if (b->len + n > MAX_BYTES || buf_append(b, tmp, n) < 0) {
                ok = 0;
                break;
            }
        }
        fclose(lf);
        return ok && b->len > 100;
    }
    const char *hdr[] = {"User-Agent: Mozilla/5.0", NULL};
    http_req_t r;
    memset(&r, 0, sizeof r);
    r.url = url;
    r.headers = hdr;
    r.range_start = -1;
    r.range_end = -1;
    r.timeout_s = 15;
    r.cancel = &g_never_cancel;
    long status = 0;
    int rc = http_fetch(h, &r, b, MAX_BYTES, &status);
    return rc == HTTP_OK && status == 200 && b->len > 100;
}

static int worker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    http_t *h = http_new();
    if (!h) {
        log_msg("thumbs: http_new failed");
        return 0;
    }
    while (!T.quit) {
        char url[300];
        int pick = -1;
        uint32_t now = now_ms();
        mx_lock(&T.mx);
        for (int i = 0; i < CACHE; i++) {
            entry_t *e = &T.e[i];
            if (e->state != T_QUEUED) continue;
            if (now - e->used > 2500) {
                e->state = T_FREE;
                continue;
            }
            if (pick < 0 || (int32_t)(e->used - T.e[pick].used) > 0) pick = i;
        }
        if (pick >= 0) {
            T.e[pick].state = T_FETCHING;
            str_set(url, sizeof url, T.e[pick].url);
        }
        mx_unlock(&T.mx);
        if (pick < 0) {
            sleep_ms(30);
            continue;
        }
        buf_t b;
        buf_init(&b);
        int ok = fetch_url(h, url, &b);
        mx_lock(&T.mx);
        entry_t *e = &T.e[pick];
        e->at = now_ms();
        if (ok) {
            e->data = (uint8_t *)b.data;
            e->len = b.len;
            b.data = NULL;
            e->state = T_FETCHED;
        } else {
            e->state = T_FAILED;
            if (g_fail_logs < 8) {
                g_fail_logs++;
                log_msg("thumbs: fetch failed for %.80s", url);
            }
        }
        mx_unlock(&T.mx);
        buf_free(&b);
    }
    http_free(h);
    return 0;
}

void thumbs_init(void) {
    mx_init(&T.mx, "ytm_thumbs");
    for (int i = 0; i < FETCH_THREADS; i++) {
        g_th[i] = sceKernelCreateThread("ytm_thumb", worker, 0x10000100, 0x20000, 0, 0, NULL);
        if (g_th[i] >= 0) sceKernelStartThread(g_th[i], 0, NULL);
        else log_msg("thumbs: thread create failed %08x", (unsigned)g_th[i]);
    }
}

void thumbs_shutdown(void) {
    T.quit = 1;
    sleep_ms(60);
    vita2d_wait_rendering_done();
    mx_lock(&T.mx);
    for (int i = 0; i < CACHE; i++) {
        if (T.e[i].tex) vita2d_free_texture(T.e[i].tex);
        T.e[i].tex = NULL;
    }
    mx_unlock(&T.mx);
}

void thumbs_pump(void) {
    int pick = -1;
    uint8_t *data = NULL;
    size_t len = 0;
    mx_lock(&T.mx);
    for (int i = 0; i < CACHE; i++) {
        if (T.e[i].state != T_FETCHED) continue;
        if (pick < 0 || (int32_t)(T.e[i].used - T.e[pick].used) > 0) pick = i;
    }
    if (pick >= 0) {
        data = T.e[pick].data;
        len = T.e[pick].len;
        T.e[pick].data = NULL;
        T.e[pick].state = T_DECODING;
    }
    mx_unlock(&T.mx);
    if (pick >= 0) {
        vita2d_texture *tex = vita2d_load_JPEG_buffer(data, (unsigned long)len);
        free(data);
        if (tex) vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        if (!tex) log_msg("thumbs: jpeg decode failed (%zu bytes)", len);
        mx_lock(&T.mx);
        T.e[pick].at = now_ms();
        T.e[pick].tex = tex;
        T.e[pick].state = tex ? T_READY : T_FAILED;
        mx_unlock(&T.mx);
    }

    uint32_t now = now_ms();
    int freed = 0;
    mx_lock(&T.mx);
    int nfree = 0;
    for (int i = 0; i < CACHE; i++)
        if (T.e[i].state == T_FREE) nfree++;
    while (nfree < MIN_FREE) {
        int v = -1;
        for (int i = 0; i < CACHE; i++) {
            entry_t *e = &T.e[i];
            if (e->state != T_READY && e->state != T_FAILED) continue;
            if (now - e->used < 1000) continue;
            if (v < 0 || (int32_t)(e->used - T.e[v].used) < 0) v = i;
        }
        if (v < 0) break;
        if (!freed) {
            mx_unlock(&T.mx);
            vita2d_wait_rendering_done();
            mx_lock(&T.mx);
            freed = 1;
            if (T.e[v].state != T_READY && T.e[v].state != T_FAILED) continue;
        }
        if (T.e[v].tex) vita2d_free_texture(T.e[v].tex);
        T.e[v].tex = NULL;
        T.e[v].state = T_FREE;
        nfree++;
    }
    mx_unlock(&T.mx);
}

vita2d_texture *thumbs_get(const track_t *t, float *fade) {
    *fade = 1.0f;
    if (!t->id[0] || strncmp(t->id, "ui:", 3) == 0) return NULL;
    uint32_t now = now_ms();
    vita2d_texture *tex = NULL;
    mx_lock(&T.mx);
    int i = find_locked(t->id);
    if (i >= 0) {
        entry_t *e = &T.e[i];
        e->used = now;
        if (e->state == T_READY) {
            tex = e->tex;
            float f = (float)(now - e->at) / 220.0f;
            *fade = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        } else if (e->state == T_FAILED && now - e->at > 30000) {
            e->state = T_QUEUED;
            e->at = now;
        }
        mx_unlock(&T.mx);
        return tex;
    }
    int slot = free_slot_locked();
    mx_unlock(&T.mx);
    if (slot < 0) return NULL;
    char url[300];
    build_url(t, url, sizeof url);
    mx_lock(&T.mx);
    if (find_locked(t->id) < 0) {
        slot = T.e[slot].state == T_FREE ? slot : free_slot_locked();
        if (slot >= 0) {
            entry_t *e = &T.e[slot];
            memset(e, 0, sizeof *e);
            e->state = T_QUEUED;
            str_set(e->id, sizeof e->id, t->id);
            str_set(e->url, sizeof e->url, url);
            e->used = now;
            e->at = now;
        }
    }
    mx_unlock(&T.mx);
    return NULL;
}
