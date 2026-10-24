#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <psp2/net/net.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#define NET_POOL (1024 * 1024)

struct http {
    CURL *c;
    char err[CURL_ERROR_SIZE + 64];
};

typedef struct {
    http_t *h;
    const http_req_t *req;
    int aborted;
    int checked;
} wctx_t;

static char g_cainfo[160];
static int g_insecure;
static int g_net_ready;

#define SSL_LOCKS 64
#define SSL_LOCK_FLAG 1

extern void CRYPTO_set_locking_callback(void (*func)(int mode, int type, const char *file, int line));
extern void CRYPTO_set_id_callback(unsigned long (*func)(void));

typedef struct {
    void (*seed)(const void *buf, int num);
    int (*bytes)(unsigned char *buf, int num);
    void (*cleanup)(void);
    void (*add)(const void *buf, int num, double entropy);
    int (*pseudorand)(unsigned char *buf, int num);
    int (*status)(void);
} ssl_rand_method_t;

extern int RAND_set_rand_method(const ssl_rand_method_t *meth);

static mutex_t g_ssl_locks[SSL_LOCKS];

static void ssl_rand_seed(const void *buf, int num) {
    (void)buf;
    (void)num;
}

static void ssl_rand_add(const void *buf, int num, double entropy) {
    (void)buf;
    (void)num;
    (void)entropy;
}

static void ssl_rand_cleanup(void) {}

static int ssl_rand_bytes(unsigned char *buf, int num) {
    if (num < 0) return 0;
    int off = 0;
    while (off < num) {
        int chunk = num - off;
        if (chunk > 32) chunk = 32;
        int ok = 0;
        for (int tries = 0; tries < 4 && !ok; tries++) ok = sceKernelGetRandomNumber(buf + off, (SceSize)chunk) >= 0;
        if (!ok) return 0;
        off += chunk;
    }
    return 1;
}

static int ssl_rand_status(void) {
    return 1;
}

static const ssl_rand_method_t g_ssl_rand = {
    ssl_rand_seed,
    ssl_rand_bytes,
    ssl_rand_cleanup,
    ssl_rand_add,
    ssl_rand_bytes,
    ssl_rand_status,
};

static void ssl_locking_cb(int mode, int type, const char *file, int line) {
    (void)file;
    (void)line;
    if (type < 0 || type >= SSL_LOCKS) return;
    if (mode & SSL_LOCK_FLAG) mx_lock(&g_ssl_locks[type]);
    else mx_unlock(&g_ssl_locks[type]);
}

static unsigned long ssl_thread_id_cb(void) {
    return (unsigned long)sceKernelGetThreadId();
}

static void ssl_threading_init(void) {
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < SSL_LOCKS; i++) mx_init(&g_ssl_locks[i], "ytm_ssl");
    CRYPTO_set_id_callback(ssl_thread_id_cb);
    CRYPTO_set_locking_callback(ssl_locking_cb);
    RAND_set_rand_method(&g_ssl_rand);
}

int net_init(int insecure) {
    g_insecure = insecure;
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    static SceNetInitParam param;
    void *pool = malloc(NET_POOL);
    if (!pool) return -1;
    param.memory = pool;
    param.size = NET_POOL;
    param.flags = 0;
    int r = sceNetInit(&param);
    if (r < 0) log_msg("sceNetInit failed %08x", (unsigned)r);
    r = sceNetCtlInit();
    if (r < 0) log_msg("sceNetCtlInit failed %08x", (unsigned)r);
    ssl_threading_init();
    CURLcode cc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (cc != CURLE_OK) {
        log_msg("curl_global_init failed %d", (int)cc);
        return -1;
    }
    const char *candidates[] = {
        DATA_DIR "/cacert.pem",
        "app0:data/cacert.pem",
        "vs0:data/external/cert/CA_LIST.cer",
        NULL,
    };
    g_cainfo[0] = '\0';
    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            snprintf(g_cainfo, sizeof g_cainfo, "%s", candidates[i]);
            break;
        }
    }
    log_msg("net ready, ca=%s insecure=%d curl=%s", g_cainfo[0] ? g_cainfo : "none", g_insecure,
            curl_version());
    g_net_ready = 1;
    return 0;
}

int net_wifi_connected(void) {
    int state = 0;
    if (sceNetCtlInetGetState(&state) < 0) return 0;
    return state == SCE_NETCTL_STATE_CONNECTED;
}

http_t *http_new(void) {
    if (!g_net_ready) return NULL;
    http_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->c = curl_easy_init();
    if (!h->c) {
        free(h);
        return NULL;
    }
    return h;
}

void http_free(http_t *h) {
    if (!h) return;
    if (h->c) curl_easy_cleanup(h->c);
    free(h);
}

const char *http_last_error(http_t *h) {
    return h ? h->err : "";
}

static size_t write_cb(char *p, size_t sz, size_t n, void *ud) {
    wctx_t *w = ud;
    size_t len = sz * n;
    if (!w->checked) {
        w->checked = 1;
        long code = 0;
        curl_easy_getinfo(w->h->c, CURLINFO_RESPONSE_CODE, &code);
        if (w->req->range_start > 0 && code == 200) {
            w->aborted = 3;
            return 0;
        }
        if (w->req->fail_http_error && code >= 400) {
            w->aborted = 5;
            return 0;
        }
    }
    if (w->req->cancel && *w->req->cancel) {
        w->aborted = 1;
        return 0;
    }
    if (w->req->sink) {
        int r = w->req->sink((const uint8_t *)p, len, w->req->user);
        if (r) {
            w->aborted = r > 0 ? 4 : 5;
            return 0;
        }
    }
    return len;
}

static int xfer_cb(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)dt;
    (void)dn;
    (void)ut;
    (void)un;
    wctx_t *w = ud;
    return (w->req->cancel && *w->req->cancel) ? 1 : 0;
}

static void short_url(const char *url, char *out, size_t cap) {
    size_t n = 0;
    while (url[n] && url[n] != '?' && n + 1 < cap && n < 110) {
        out[n] = url[n];
        n++;
    }
    out[n] = '\0';
}

int http_run(http_t *h, const http_req_t *req, long *status) {
    if (status) *status = 0;
    if (!h || !h->c) return HTTP_FAILED;
    CURL *c = h->c;
    curl_easy_reset(c);
    h->err[0] = '\0';

    wctx_t w = {h, req, 0, 0};
    struct curl_slist *hl = NULL;
    if (req->headers) {
        for (int i = 0; req->headers[i]; i++) hl = curl_slist_append(hl, req->headers[i]);
    }

    curl_easy_setopt(c, CURLOPT_URL, req->url);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, h->err);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 6L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 25L);
    curl_easy_setopt(c, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 32768L);
    if (req->timeout_s > 0) curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)req->timeout_s);
    if (req->compress) curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    if (g_insecure) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    } else {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        if (g_cainfo[0]) curl_easy_setopt(c, CURLOPT_CAINFO, g_cainfo);
    }
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    if (req->post) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req->post);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)(req->post_len ? req->post_len : strlen(req->post)));
    }
    if (req->range_start >= 0) {
        char range[64];
        if (req->range_end >= 0)
            snprintf(range, sizeof range, "%lld-%lld", (long long)req->range_start, (long long)req->range_end);
        else
            snprintf(range, sizeof range, "%lld-", (long long)req->range_start);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &w);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &w);

    CURLcode res = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;
    curl_slist_free_all(hl);

    char su[128];
    short_url(req->url, su, sizeof su);

    if (w.aborted == 1 || (res == CURLE_ABORTED_BY_CALLBACK && w.aborted == 0)) return HTTP_CANCELLED;
    if (w.aborted == 3) {
        log_msg("http %s: range ignored (status %ld)", su, code);
        return HTTP_RANGE_IGNORED;
    }
    if (w.aborted == 4) return HTTP_STOPPED;
    if (w.aborted == 5) {
        if (code >= 400) log_msg("http %s: status %ld", su, code);
        return HTTP_FAILED;
    }
    if (res != CURLE_OK) {
        if (!h->err[0]) snprintf(h->err, sizeof h->err, "%s", curl_easy_strerror(res));
        log_msg("http %s: curl %d (%s) status %ld", su, (int)res, h->err, code);
        return HTTP_FAILED;
    }
    return HTTP_OK;
}

typedef struct {
    buf_t *b;
    size_t max;
} bufsink_t;

static int buf_sink(const uint8_t *d, size_t n, void *u) {
    bufsink_t *s = u;
    int stop = 0;
    if (s->max && s->b->len + n >= s->max) {
        n = s->max - s->b->len;
        stop = 1;
    }
    if (buf_append(s->b, d, n) < 0) return -1;
    return stop;
}

int http_fetch(http_t *h, const http_req_t *req, buf_t *out, size_t max_len, long *status) {
    http_req_t r = *req;
    bufsink_t s = {out, max_len};
    r.sink = buf_sink;
    r.user = &s;
    int rc = http_run(h, &r, status);
    if (rc == HTTP_STOPPED) rc = HTTP_OK;
    return rc;
}
