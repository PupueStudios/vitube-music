#include "dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include "hls.h"
#include "hlsres.h"
#include "net.h"
#include "store.h"
#include "tsdemux.h"
#include "ytm.h"

#define Q_CAP 128
#define ART_MAX_BYTES (512 * 1024)

enum {
    SEG_UNKNOWN = 0,
    SEG_TS,
    SEG_ADTS,
    SEG_ID3,
    SEG_BAD
};

typedef struct {
    ts_demux_t ts;
    int mode;
    uint8_t head[12];
    int headn;
    size_t id3_left;
    FILE *f;
    int err;
} segw_t;

static struct {
    mutex_t mx;
    track_t q[Q_CAP];
    int qn;
    int active;
    char id[16];
    char title[128];
    int pct;
    int ev_pending;
    char ev_msg[160];
    volatile int quit;
} D;

static SceUID g_th;

static const char *const HDR_VISION[] = {"User-Agent: " YTM_UA_VISION, NULL};

static void set_event(const char *fmt, const char *title) {
    mx_lock(&D.mx);
    snprintf(D.ev_msg, sizeof D.ev_msg, fmt, title);
    D.ev_pending = 1;
    mx_unlock(&D.mx);
}

static void segw_audio(const uint8_t *d, size_t n, void *u) {
    segw_t *c = u;
    if (c->err) return;
    if (fwrite(d, 1, n, c->f) != n) c->err = 1;
}

static void segw_data(segw_t *c, const uint8_t *d, size_t n) {
    while (n > 0 && !c->err) {
        if (c->mode == SEG_ID3) {
            size_t k = c->id3_left < n ? c->id3_left : n;
            d += k;
            n -= k;
            c->id3_left -= k;
            if (c->id3_left == 0) c->mode = SEG_ADTS;
        } else if (c->mode == SEG_TS) {
            ts_feed(&c->ts, d, n);
            return;
        } else if (c->mode == SEG_ADTS) {
            segw_audio(d, n, c);
            return;
        } else {
            return;
        }
    }
}

static int segw_sink(const uint8_t *d, size_t n, void *u) {
    segw_t *c = u;
    if (c->mode == SEG_UNKNOWN) {
        while (n > 0 && c->headn < (int)sizeof c->head) {
            c->head[c->headn++] = *d++;
            n--;
        }
        if (c->headn < (int)sizeof c->head) return 0;
        const uint8_t *h = c->head;
        if (h[0] == 0x47) {
            c->mode = SEG_TS;
        } else if (h[0] == 0xFF && (h[1] & 0xF0) == 0xF0) {
            c->mode = SEG_ADTS;
        } else if (h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
            size_t sz = ((size_t)(h[6] & 0x7F) << 21) | ((size_t)(h[7] & 0x7F) << 14) | ((size_t)(h[8] & 0x7F) << 7) |
                        (size_t)(h[9] & 0x7F);
            c->id3_left = 10 + sz;
            c->mode = SEG_ID3;
        } else {
            c->mode = SEG_BAD;
            c->err = 2;
            return -1;
        }
        segw_data(c, c->head, sizeof c->head);
    }
    segw_data(c, d, n);
    return c->err ? -1 : 0;
}

static int count_frames(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    enum { CAP = 64 * 1024 };
    uint8_t *buf = malloc(CAP);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    size_t len = 0;
    int frames = 0;
    int eof = 0;
    while (!eof || len > 0) {
        if (!eof && len < CAP) {
            size_t n = fread(buf + len, 1, CAP - len, fp);
            if (n == 0) eof = 1;
            len += n;
        }
        size_t pos = 0;
        int progressed = 0;
        for (;;) {
            while (pos + 1 < len && !(buf[pos] == 0xFF && (buf[pos + 1] & 0xF6) == 0xF0)) pos++;
            if (pos + 7 > len) break;
            size_t flen = (size_t)(((buf[pos + 3] & 3) << 11) | (buf[pos + 4] << 3) | (buf[pos + 5] >> 5));
            if (flen < 7) {
                pos++;
                continue;
            }
            if (pos + flen > len) break;
            if (pos + flen + 2 <= len) {
                if (!(buf[pos + flen] == 0xFF && (buf[pos + flen + 1] & 0xF6) == 0xF0)) {
                    pos++;
                    continue;
                }
            } else if (!eof) {
                break;
            }
            frames++;
            pos += flen;
            progressed = 1;
        }
        if (pos > 0) {
            memmove(buf, buf + pos, len - pos);
            len -= pos;
            progressed = 1;
        }
        if (eof && !progressed) break;
        if (eof && len < 7) break;
    }
    free(buf);
    fclose(fp);
    return frames;
}

static void save_art(http_t *h, const track_t *t) {
    char url[300];
    if (t->thumb[0]) snprintf(url, sizeof url, "%s", t->thumb);
    else snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/hqdefault.jpg", t->id);
    buf_t b;
    buf_init(&b);
    const char *hdr[] = {"User-Agent: Mozilla/5.0", NULL};
    http_req_t r;
    memset(&r, 0, sizeof r);
    r.url = url;
    r.headers = hdr;
    r.range_start = -1;
    r.range_end = -1;
    r.timeout_s = 20;
    r.cancel = &D.quit;
    long status = 0;
    int rc = http_fetch(h, &r, &b, ART_MAX_BYTES, &status);
    if (rc == HTTP_OK && status == 200 && b.len > 100) {
        char path[96];
        store_dl_path(t->id, "jpg", path, sizeof path);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            fwrite(b.data, 1, b.len, fp);
            fclose(fp);
        }
    }
    buf_free(&b);
}

static int run_download(http_t *h, const track_t *t) {
    char err[160] = "";
    hls_media_t m;
    memset(&m, 0, sizeof m);
    int rc = hlsres_resolve(h, t->id, &m, &D.quit, err, sizeof err);
    if (rc < 0) {
        log_msg("download %s: resolve failed: %s", t->id, err);
        return -1;
    }
    sceIoMkdir(DL_DIR, 0777);
    char tmp[96];
    char fin[96];
    store_dl_path(t->id, "part", tmp, sizeof tmp);
    store_dl_path(t->id, "aac", fin, sizeof fin);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        log_msg("download %s: cannot create %s", t->id, tmp);
        hls_media_free(&m);
        return -1;
    }
    setvbuf(f, NULL, _IOFBF, 64 * 1024);

    segw_t *c = calloc(1, sizeof *c);
    if (!c) {
        fclose(f);
        sceIoRemove(tmp);
        hls_media_free(&m);
        return -1;
    }
    c->f = f;

    int ok = 1;
    for (int i = 0; i < m.n && ok; i++) {
        int seg_ok = 0;
        long pos = ftell(f);
        for (int attempt = 0; attempt < 3 && !seg_ok; attempt++) {
            if (D.quit) {
                ok = 0;
                break;
            }
            fseek(f, pos, SEEK_SET);
            c->mode = SEG_UNKNOWN;
            c->headn = 0;
            c->id3_left = 0;
            c->err = 0;
            ts_init(&c->ts, segw_audio, c);
            http_req_t rq;
            memset(&rq, 0, sizeof rq);
            rq.url = m.segs[i].uri;
            rq.headers = HDR_VISION;
            rq.range_start = -1;
            rq.range_end = -1;
            rq.cancel = &D.quit;
            rq.sink = segw_sink;
            rq.user = c;
            rq.fail_http_error = 1;
            long status = 0;
            int hr = http_run(h, &rq, &status);
            if (hr == HTTP_OK && !c->err) {
                seg_ok = 1;
                break;
            }
            log_msg("download %s: segment %d attempt %d failed rc=%d status=%ld err=%d", t->id, i, attempt, hr,
                    status, c->err);
            sleep_ms(300 * (attempt + 1));
        }
        if (!seg_ok) ok = 0;
        mx_lock(&D.mx);
        D.pct = (i + 1) * 100 / m.n;
        mx_unlock(&D.mx);
    }
    int dur_ms = (int)(m.total * 1000.0);
    hls_media_free(&m);
    free(c);
    if (ok) {
        fflush(f);
        long end = ftell(f);
        fclose(f);
        f = NULL;
        if (end <= 0) ok = 0;
    } else {
        fclose(f);
        f = NULL;
    }
    if (!ok) {
        sceIoRemove(tmp);
        return -1;
    }

    int frames = count_frames(tmp);
    if (frames < 10) {
        log_msg("download %s: only %d audio frames found", t->id, frames);
        sceIoRemove(tmp);
        return -1;
    }
    sceIoRemove(fin);
    if (sceIoRename(tmp, fin) < 0) {
        log_msg("download %s: rename failed", t->id);
        sceIoRemove(tmp);
        return -1;
    }
    save_art(h, t);
    store_dl_add(t, frames, dur_ms);
    log_msg("download %s done: %d frames, %d ms", t->id, frames, dur_ms);
    return 0;
}

static int worker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    http_t *h = http_new();
    if (!h) {
        log_msg("dl: http_new failed");
        return 0;
    }
    while (!D.quit) {
        track_t t;
        int go = 0;
        mx_lock(&D.mx);
        if (D.qn > 0) {
            t = D.q[0];
            memmove(&D.q[0], &D.q[1], sizeof(track_t) * (size_t)(D.qn - 1));
            D.qn--;
            D.active = 1;
            D.pct = 0;
            str_set(D.id, sizeof D.id, t.id);
            str_set(D.title, sizeof D.title, t.title);
            go = 1;
        }
        mx_unlock(&D.mx);
        if (!go) {
            sleep_ms(120);
            continue;
        }
        if (store_dl_find(t.id, NULL, NULL)) {
            mx_lock(&D.mx);
            D.active = 0;
            mx_unlock(&D.mx);
            continue;
        }
        int rc = run_download(h, &t);
        mx_lock(&D.mx);
        D.active = 0;
        D.id[0] = '\0';
        mx_unlock(&D.mx);
        if (D.quit) break;
        if (rc == 0) set_event("Downloaded: %s", t.title);
        else set_event("Download failed: %s", t.title);
    }
    http_free(h);
    return 0;
}

void dl_init(void) {
    mx_init(&D.mx, "ytm_dl");
    sceIoMkdir(DL_DIR, 0777);
    g_th = sceKernelCreateThread("ytm_dl", worker, 0x10000100, 0x40000, 0, 0, NULL);
    if (g_th >= 0) sceKernelStartThread(g_th, 0, NULL);
    else log_msg("dl: thread create failed %08x", (unsigned)g_th);
}

void dl_shutdown(void) {
    D.quit = 1;
}

int dl_enqueue(const track_t *t) {
    if (store_dl_find(t->id, NULL, NULL)) return 0;
    int added = 0;
    mx_lock(&D.mx);
    int dup = D.active && strcmp(D.id, t->id) == 0;
    for (int i = 0; i < D.qn && !dup; i++)
        if (strcmp(D.q[i].id, t->id) == 0) dup = 1;
    if (!dup && D.qn < Q_CAP) {
        D.q[D.qn++] = *t;
        added = 1;
    }
    mx_unlock(&D.mx);
    return added;
}

void dl_status(dl_status_t *s) {
    mx_lock(&D.mx);
    s->active = D.active;
    s->pct = D.pct;
    s->pending = D.qn;
    str_set(s->title, sizeof s->title, D.title);
    mx_unlock(&D.mx);
}

int dl_take_event(char *out, size_t cap) {
    int r = 0;
    mx_lock(&D.mx);
    if (D.ev_pending) {
        str_set(out, cap, D.ev_msg);
        D.ev_pending = 0;
        r = 1;
    }
    mx_unlock(&D.mx);
    return r;
}
