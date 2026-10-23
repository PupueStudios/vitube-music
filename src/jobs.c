#include "jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "net.h"
#include "player.h"
#include "ytm.h"

#define RADIO_MAX 40
#define ART_MAX_BYTES (512 * 1024)

static mutex_t g_mx;
static volatile int g_quit;
static volatile int g_never_cancel;

static struct {
    int req_pending;
    int gen;
    int busy;
    int n;
    char query[160];
    char err[128];
    track_t items[SEARCH_MAX];
} S;

static struct {
    int pending;
    int busy;
    char id[16];
    char last[16];
} R;

static struct {
    int pending;
    char url[256];
    char id[16];
    uint8_t *data;
    size_t len;
    char out_id[16];
} A;

static SceUID g_th_main;
static SceUID g_th_art;

void jobs_search(const char *query) {
    mx_lock(&g_mx);
    S.gen++;
    S.busy = 1;
    S.n = 0;
    S.err[0] = '\0';
    str_set(S.query, sizeof S.query, query);
    S.req_pending = 1;
    mx_unlock(&g_mx);
}

int jobs_search_busy(void) {
    mx_lock(&g_mx);
    int b = S.busy;
    mx_unlock(&g_mx);
    return b;
}

int jobs_search_count(void) {
    mx_lock(&g_mx);
    int n = S.n;
    mx_unlock(&g_mx);
    return n;
}

int jobs_search_get(int i, track_t *t) {
    int ok = 0;
    mx_lock(&g_mx);
    if (i >= 0 && i < S.n) {
        *t = S.items[i];
        ok = 1;
    }
    mx_unlock(&g_mx);
    return ok;
}

void jobs_search_error(char *out, size_t cap) {
    mx_lock(&g_mx);
    str_set(out, cap, S.err);
    mx_unlock(&g_mx);
}

void jobs_search_query(char *out, size_t cap) {
    mx_lock(&g_mx);
    str_set(out, cap, S.query);
    mx_unlock(&g_mx);
}

void jobs_radio(const char *video_id) {
    mx_lock(&g_mx);
    if (!R.busy && !R.pending && strcmp(R.last, video_id) != 0) {
        str_set(R.id, sizeof R.id, video_id);
        R.pending = 1;
    }
    mx_unlock(&g_mx);
}

void jobs_art(const char *url, const char *id) {
    mx_lock(&g_mx);
    str_set(A.url, sizeof A.url, url);
    str_set(A.id, sizeof A.id, id);
    A.pending = 1;
    mx_unlock(&g_mx);
}

int jobs_art_take(char *id, uint8_t **data, size_t *len) {
    int ok = 0;
    mx_lock(&g_mx);
    if (A.data) {
        str_set(id, 16, A.out_id);
        *data = A.data;
        *len = A.len;
        A.data = NULL;
        A.len = 0;
        ok = 1;
    }
    mx_unlock(&g_mx);
    return ok;
}

static void run_search(http_t *h, const char *query, int gen) {
    track_t *tmp = malloc(sizeof(track_t) * SEARCH_MAX);
    char err[128] = "";
    int n = -1;
    if (tmp) n = ytm_search(h, query, tmp, SEARCH_MAX, &g_never_cancel, err, sizeof err);
    mx_lock(&g_mx);
    if (gen == S.gen) {
        if (n > 0) {
            memcpy(S.items, tmp, sizeof(track_t) * (size_t)n);
            S.n = n;
        } else {
            S.n = 0;
            str_set(S.err, sizeof S.err, err[0] ? err : "Search failed");
        }
        S.busy = 0;
    }
    mx_unlock(&g_mx);
    free(tmp);
}

static void run_radio(http_t *h, const char *id) {
    track_t *tmp = malloc(sizeof(track_t) * RADIO_MAX);
    char err[128] = "";
    int n = -1;
    if (tmp) n = ytm_radio(h, id, tmp, RADIO_MAX, &g_never_cancel, err, sizeof err);
    if (n > 0) player_queue_append(tmp, n);
    else log_msg("radio failed for %s: %s", id, err);
    mx_lock(&g_mx);
    str_set(R.last, sizeof R.last, id);
    R.busy = 0;
    mx_unlock(&g_mx);
    free(tmp);
}

static int main_worker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    http_t *h = http_new();
    if (!h) {
        log_msg("jobs: http_new failed");
        return 0;
    }
    while (!g_quit) {
        char query[160];
        char rid[16];
        int gen = 0;
        int do_search = 0;
        int do_radio = 0;
        mx_lock(&g_mx);
        if (S.req_pending) {
            S.req_pending = 0;
            gen = S.gen;
            str_set(query, sizeof query, S.query);
            do_search = 1;
        } else if (R.pending) {
            R.pending = 0;
            R.busy = 1;
            str_set(rid, sizeof rid, R.id);
            do_radio = 1;
        }
        mx_unlock(&g_mx);
        if (do_search) run_search(h, query, gen);
        else if (do_radio) run_radio(h, rid);
        else sleep_ms(30);
    }
    http_free(h);
    return 0;
}

static int art_worker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    http_t *h = http_new();
    if (!h) return 0;
    while (!g_quit) {
        char url[256];
        char id[16];
        int go = 0;
        mx_lock(&g_mx);
        if (A.pending) {
            A.pending = 0;
            str_set(url, sizeof url, A.url);
            str_set(id, sizeof id, A.id);
            go = 1;
        }
        mx_unlock(&g_mx);
        if (!go) {
            sleep_ms(40);
            continue;
        }
        buf_t b;
        buf_init(&b);
        long status = 0;
        int rc = HTTP_FAILED;
        if (starts_with(url, "ux0:")) {
            FILE *lf = fopen(url, "rb");
            if (lf) {
                uint8_t tmp[4096];
                size_t n;
                int okread = 1;
                while ((n = fread(tmp, 1, sizeof tmp, lf)) > 0) {
                    if (b.len + n > ART_MAX_BYTES || buf_append(&b, tmp, n) < 0) {
                        okread = 0;
                        break;
                    }
                }
                fclose(lf);
                if (okread) {
                    rc = HTTP_OK;
                    status = 200;
                }
            }
        } else {
            const char *hdr[] = {"User-Agent: Mozilla/5.0", NULL};
            http_req_t r;
            memset(&r, 0, sizeof r);
            r.url = url;
            r.headers = hdr;
            r.range_start = -1;
            r.range_end = -1;
            r.timeout_s = 20;
            r.cancel = &g_never_cancel;
            rc = http_fetch(h, &r, &b, ART_MAX_BYTES, &status);
        }
        if (rc == HTTP_OK && status == 200 && b.len > 100) {
            mx_lock(&g_mx);
            if (!A.pending) {
                free(A.data);
                A.data = (uint8_t *)b.data;
                A.len = b.len;
                str_set(A.out_id, sizeof A.out_id, id);
                b.data = NULL;
            }
            mx_unlock(&g_mx);
        } else {
            log_msg("art fetch failed rc=%d status=%ld", rc, status);
        }
        buf_free(&b);
    }
    http_free(h);
    return 0;
}

void jobs_init(void) {
    mx_init(&g_mx, "ytm_jobs");
    g_th_main = sceKernelCreateThread("ytm_jobs", main_worker, 0x10000100, 0x40000, 0, 0, NULL);
    g_th_art = sceKernelCreateThread("ytm_art", art_worker, 0x10000100, 0x40000, 0, 0, NULL);
    sceKernelStartThread(g_th_main, 0, NULL);
    sceKernelStartThread(g_th_art, 0, NULL);
}

void jobs_shutdown(void) {
    g_quit = 1;
}
