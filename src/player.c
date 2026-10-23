#include "player.h"

#include <stdio.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include "aac.h"
#include "hls.h"
#include "hlsres.h"
#include "jobs.h"
#include "net.h"
#include "store.h"
#include "tsdemux.h"
#include "ytm.h"

#define RING_FRAMES (1 << 18)
#define RING_MASK (RING_FRAMES - 1)
#define GRAIN 1024
#define OUT_RATE 48000
#define QUEUE_CAP 1000
#define PCM_TMP_FRAMES 8192
#define PCM_OUT_FRAMES 16384
#define OUT_BUFS 4
#define PREBUFFER_FRAMES 24000
#define RS_TAPS 32
#define RS_HALF (RS_TAPS / 2)
#define RS_PHASES 256

typedef struct {
    volatile int cancel;
} sess_t;

typedef enum {
    RES_NONE = 0,
    RES_HLS,
    RES_FILE
} res_kind_t;

typedef struct {
    res_kind_t kind;
    char id[16];
    hls_media_t hls;
    int duration_ms;
    int frames;
} resolved_t;

enum {
    FEED_OK = 0,
    FEED_CANCEL,
    FEED_FAIL_EARLY,
    FEED_FAIL
};

static struct {
    mutex_t mx;
    int16_t *buf;
    uint32_t wr;
    uint32_t rd;
    uint32_t gen;
    int req_rate;
    int eof;
    int finished;
    int paused;
    uint64_t played;
    int base_ms;
} R;

static struct {
    mutex_t mx;
    track_t *q;
    int qn;
    int cur;
    int req_pending;
    int req_idx;
    int req_start;
    sess_t *sess;
    pl_state_t state;
    char msg[160];
    int duration_ms;
    int fail_streak;
    volatile int quit;
} P;

static uint32_t g_gen;

static const char *const HDR_VISION[] = {"User-Agent: " YTM_UA_VISION, NULL};

static void ring_begin(uint32_t gen, int base_ms) {
    mx_lock(&R.mx);
    R.wr = 0;
    R.rd = 0;
    R.gen = gen;
    R.eof = 0;
    R.finished = 0;
    R.played = 0;
    R.base_ms = base_ms;
    mx_unlock(&R.mx);
}

static void ring_set_rate(uint32_t gen, int rate) {
    mx_lock(&R.mx);
    if (R.gen == gen) R.req_rate = rate;
    mx_unlock(&R.mx);
}

static void ring_set_eof(uint32_t gen) {
    mx_lock(&R.mx);
    if (R.gen == gen) R.eof = 1;
    mx_unlock(&R.mx);
}

static int ring_finished(uint32_t gen) {
    mx_lock(&R.mx);
    int f = R.gen == gen && R.finished;
    mx_unlock(&R.mx);
    return f;
}

static int ring_write(uint32_t gen, const int16_t *pcm, int frames, volatile int *cancel) {
    while (frames > 0) {
        if (*cancel) return -1;
        mx_lock(&R.mx);
        if (R.gen != gen) {
            mx_unlock(&R.mx);
            return -1;
        }
        uint32_t freef = RING_FRAMES - (R.wr - R.rd);
        uint32_t n = (uint32_t)frames < freef ? (uint32_t)frames : freef;
        if (n > 0) {
            uint32_t w = R.wr & RING_MASK;
            uint32_t first = RING_FRAMES - w;
            if (first > n) first = n;
            memcpy(R.buf + w * 2, pcm, (size_t)first * 4);
            if (n > first) memcpy(R.buf, pcm + first * 2, (size_t)(n - first) * 4);
            R.wr += n;
        }
        mx_unlock(&R.mx);
        if (n == 0) {
            sleep_ms(8);
        } else {
            pcm += n * 2;
            frames -= (int)n;
        }
    }
    return 0;
}

static int audio_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int16_t *bufs[OUT_BUFS];
    void *raw = memalign(64, (size_t)OUT_BUFS * GRAIN * 4);
    if (!raw) {
        log_msg("audio: out of memory");
        return 0;
    }
    memset(raw, 0, (size_t)OUT_BUFS * GRAIN * 4);
    for (int i = 0; i < OUT_BUFS; i++) bufs[i] = (int16_t *)raw + (size_t)i * GRAIN * 2;
    int bi = 0;
    int primed = 0;
    int port = -1;
    int cur_rate = 0;
    while (!P.quit) {
        mx_lock(&R.mx);
        int want = OUT_RATE;
        int paused = R.paused;
        mx_unlock(&R.mx);

        if (port < 0 || want != cur_rate) {
            if (port >= 0) sceAudioOutReleasePort(port);
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, want, SCE_AUDIO_OUT_MODE_STEREO);
            if (port < 0) {
                log_msg("audio open failed %08x (rate %d)", (unsigned)port, want);
                port = -1;
                cur_rate = 0;
                sleep_ms(200);
                continue;
            }
            log_msg("audio port opened at %d Hz", want);
            cur_rate = want;
        }

        int16_t *out = bufs[bi];
        int have = 0;
        if (!paused) {
            mx_lock(&R.mx);
            uint32_t avail = R.wr - R.rd;
            if (!primed) {
                if (avail >= PREBUFFER_FRAMES || (R.eof && avail > 0)) primed = 1;
            } else if (avail < GRAIN && !R.eof) {
                primed = 0;
            }
            uint32_t n = 0;
            if (primed) n = avail >= GRAIN ? GRAIN : avail;
            if (n > 0) {
                uint32_t r = R.rd & RING_MASK;
                uint32_t first = RING_FRAMES - r;
                if (first > n) first = n;
                memcpy(out, R.buf + r * 2, (size_t)first * 4);
                if (n > first) memcpy(out + first * 2, R.buf, (size_t)(n - first) * 4);
                if (n < GRAIN) memset(out + n * 2, 0, (GRAIN - n) * 4);
                R.rd += n;
                R.played += n;
                have = 1;
            }
            if (R.eof && R.wr == R.rd) R.finished = 1;
            mx_unlock(&R.mx);
        }
        if (!have) memset(out, 0, GRAIN * 4);
        sceAudioOutOutput(port, out);
        bi = (bi + 1) % OUT_BUFS;
    }
    if (port >= 0) sceAudioOutReleasePort(port);
    free(raw);
    return 0;
}

typedef struct {
    sess_t *s;
    uint32_t gen;
    int rate;
    int skip;
    int skip_total;
    int skip_ms;
    int seg_frames;
    int produced;
    int failed;
    int16_t *tmp_a;
    int16_t *tmp_b;
    int16_t *work;
    int64_t rs_pos;
    int rs_src;
} feed_t;

static float g_rs_tab[RS_PHASES + 1][RS_TAPS];
static int g_rs_ready;
static float g_rs_cut;

static void rs_build(float cutoff) {
    const double pi = 3.14159265358979323846;
    for (int ph = 0; ph <= RS_PHASES; ph++) {
        double d = (double)ph / RS_PHASES;
        double sum = 0;
        double tmp[RS_TAPS];
        for (int k = 0; k < RS_TAPS; k++) {
            double x = (double)(k - (RS_HALF - 1)) - d;
            double t = 2.0 * cutoff * x;
            double sinc = fabs(t) < 1e-9 ? 1.0 : sin(pi * t) / (pi * t);
            double wpos = x / (double)RS_HALF;
            double win = fabs(wpos) >= 1.0 ? 0.0 : 0.42 + 0.5 * cos(pi * wpos) + 0.08 * cos(2.0 * pi * wpos);
            tmp[k] = 2.0 * cutoff * sinc * win;
            sum += tmp[k];
        }
        for (int k = 0; k < RS_TAPS; k++) g_rs_tab[ph][k] = (float)(tmp[k] / sum);
    }
    g_rs_cut = cutoff;
    g_rs_ready = 1;
}

static int resample(feed_t *f, const int16_t *in, int nin, int rin, int16_t *out, int cap, int rout) {
    float cutoff = rout >= rin ? 0.46f : 0.46f * (float)rout / (float)rin;
    if (!g_rs_ready || fabsf(g_rs_cut - cutoff) > 1e-6f) rs_build(cutoff);
    if (f->rs_src != rin) {
        f->rs_src = rin;
        f->rs_pos = 0;
        memset(f->work, 0, (size_t)RS_TAPS * 4);
    }
    memcpy(f->work + RS_TAPS * 2, in, (size_t)nin * 4);
    int64_t step = ((int64_t)rin << 16) / rout;
    int n = 0;
    while (n < cap) {
        int64_t i0 = f->rs_pos >> 16;
        if (i0 + RS_HALF > nin - 1) break;
        int ph = (int)(((f->rs_pos & 0xFFFF) * RS_PHASES + 0x8000) >> 16);
        const float *h = g_rs_tab[ph];
        const int16_t *w = f->work + (size_t)(RS_TAPS + i0 - (RS_HALF - 1)) * 2;
        float al = 0.0f;
        float ar = 0.0f;
        for (int k = 0; k < RS_TAPS; k++) {
            al += h[k] * (float)w[k * 2];
            ar += h[k] * (float)w[k * 2 + 1];
        }
        int l = (int)lrintf(al);
        int r = (int)lrintf(ar);
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        out[n * 2] = (int16_t)l;
        out[n * 2 + 1] = (int16_t)r;
        n++;
        f->rs_pos += step;
    }
    f->rs_pos -= (int64_t)nin << 16;
    memmove(f->work, f->work + (size_t)nin * 2, (size_t)RS_TAPS * 4);
    return n;
}

static void mark_playing(void) {
    mx_lock(&R.mx);
    int paused = R.paused;
    mx_unlock(&R.mx);
    mx_lock(&P.mx);
    if (P.state == PL_LOADING) P.state = paused ? PL_PAUSED : PL_PLAYING;
    mx_unlock(&P.mx);
}

static void pcm_cb(const int16_t *pcm, int frames, int ch, int rate, void *u) {
    feed_t *f = u;
    if (f->failed) return;
    if (f->s->cancel) {
        f->failed = 1;
        return;
    }
    if (frames > PCM_TMP_FRAMES / 2) frames = PCM_TMP_FRAMES / 2;
    if (frames <= 0) return;
    if (!f->rate) {
        f->rate = OUT_RATE;
        f->skip_total = (int)((int64_t)f->skip_ms * OUT_RATE / 1000);
        f->skip = f->skip_total;
        ring_set_rate(f->gen, OUT_RATE);
        log_msg("pcm: %d Hz, %d ch -> %d Hz", rate, ch, OUT_RATE);
    }
    const int16_t *src = pcm;
    int n = frames;
    if (ch == 1) {
        for (int i = 0; i < n; i++) {
            f->tmp_a[i * 2] = pcm[i];
            f->tmp_a[i * 2 + 1] = pcm[i];
        }
        src = f->tmp_a;
    }
    if (rate != OUT_RATE) {
        n = resample(f, src, n, rate, f->tmp_b, PCM_OUT_FRAMES, OUT_RATE);
        src = f->tmp_b;
    }
    f->seg_frames += n;
    if (f->skip > 0) {
        int d = f->skip < n ? f->skip : n;
        f->skip -= d;
        src += d * 2;
        n -= d;
    }
    if (n > 0) {
        if (ring_write(f->gen, src, n, &f->s->cancel) < 0) {
            f->failed = 1;
            return;
        }
        if (f->produced == 0) {
            log_msg("first audio written (%d frames)", n);
            mark_playing();
        }
        f->produced += n;
    }
}

static feed_t *feed_new(sess_t *s, uint32_t gen, int skip_ms) {
    feed_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->tmp_a = malloc(PCM_TMP_FRAMES * 4);
    f->tmp_b = malloc(PCM_OUT_FRAMES * 4);
    f->work = calloc((size_t)(RS_TAPS + PCM_TMP_FRAMES) * 2, sizeof(int16_t));
    if (!f->tmp_a || !f->tmp_b || !f->work) {
        free(f->tmp_a);
        free(f->tmp_b);
        free(f->work);
        free(f);
        return NULL;
    }
    f->s = s;
    f->gen = gen;
    f->skip_ms = skip_ms < 0 ? 0 : skip_ms;
    return f;
}

static void feed_free(feed_t *f) {
    if (!f) return;
    free(f->tmp_a);
    free(f->tmp_b);
    free(f->work);
    free(f);
}

enum {
    SEG_UNKNOWN = 0,
    SEG_TS,
    SEG_ADTS,
    SEG_ID3,
    SEG_BAD
};

typedef struct {
    ts_demux_t ts;
    aac_t *aac;
    feed_t *f;
    int mode;
    uint8_t head[12];
    int headn;
    size_t id3_left;
} tsctx_t;

static void ts_audio_cb(const uint8_t *d, size_t n, void *u) {
    tsctx_t *c = u;
    if (c->f->failed) return;
    if (aac_push_adts(c->aac, d, n) < 0) c->f->failed = 2;
}

static void seg_data(tsctx_t *c, const uint8_t *d, size_t n) {
    while (n > 0 && !c->f->failed) {
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
            if (aac_push_adts(c->aac, d, n) < 0) c->f->failed = 2;
            return;
        } else {
            return;
        }
    }
}

static int ts_sink(const uint8_t *d, size_t n, void *u) {
    tsctx_t *c = u;
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
            log_msg("hls segment: unsupported container, first bytes %02x %02x %02x %02x %02x %02x %02x %02x", h[0],
                    h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
            c->f->failed = 3;
            return -1;
        }
        log_msg("hls segment format: %s", c->mode == SEG_TS ? "mpeg-ts" : (c->mode == SEG_ADTS ? "adts" : "id3+adts"));
        seg_data(c, c->head, sizeof c->head);
    }
    seg_data(c, d, n);
    return c->f->failed ? -1 : 0;
}

static int feed_hls(http_t *h, resolved_t *r, int start_ms, sess_t *s, uint32_t gen) {
    int seg_start = 0;
    int first = hls_find_segment(&r->hls, start_ms, &seg_start);
    feed_t *f = feed_new(s, gen, start_ms - seg_start);
    if (!f) return FEED_FAIL_EARLY;
    tsctx_t *c = calloc(1, sizeof *c);
    if (c) {
        c->f = f;
        c->aac = aac_new(pcm_cb, f);
    }
    if (!c || !c->aac) {
        free(c);
        feed_free(f);
        return FEED_FAIL_EARLY;
    }
    int result = FEED_OK;
    for (int i = first; i < r->hls.n; i++) {
        int ok = 0;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            if (s->cancel || f->failed == 1) {
                result = FEED_CANCEL;
                goto done;
            }
            f->seg_frames = 0;
            f->failed = 0;
            c->mode = SEG_UNKNOWN;
            c->headn = 0;
            c->id3_left = 0;
            aac_reset_adts(c->aac);
            ts_init(&c->ts, ts_audio_cb, c);
            http_req_t rq;
            memset(&rq, 0, sizeof rq);
            rq.url = r->hls.segs[i].uri;
            rq.headers = HDR_VISION;
            rq.range_start = -1;
            rq.range_end = -1;
            rq.cancel = &s->cancel;
            rq.sink = ts_sink;
            rq.user = c;
            rq.fail_http_error = 1;
            long status = 0;
            int rc = http_run(h, &rq, &status);
            if (s->cancel || rc == HTTP_CANCELLED) {
                result = FEED_CANCEL;
                goto done;
            }
            if (f->failed == 1) {
                result = FEED_CANCEL;
                goto done;
            }
            if (f->failed == 3) {
                result = FEED_FAIL_EARLY;
                goto done;
            }
            if (rc == HTTP_OK && !f->failed) {
                if (f->produced == 0 && i == first) {
                    log_msg("hls: first segment produced no audio");
                    result = FEED_FAIL_EARLY;
                    goto done;
                }
                ok = 1;
                break;
            }
            log_msg("hls seg %d attempt %d failed rc=%d status=%ld dec=%d", i, attempt, rc, status, f->failed);
            f->skip = (i == first && f->skip_total > f->seg_frames) ? f->skip_total : f->seg_frames;
            sleep_ms(300 * (attempt + 1));
        }
        if (!ok) {
            result = f->produced > 0 ? FEED_FAIL : FEED_FAIL_EARLY;
            goto done;
        }
        f->skip_total = 0;
    }
done:
    aac_free(c->aac);
    free(c);
    feed_free(f);
    return result;
}

static int adts_skip(FILE *fp, int frames) {
    uint8_t hd[7];
    int i = 0;
    int bad = 0;
    while (i < frames) {
        if (fread(hd, 1, 7, fp) != 7) return -1;
        long flen = ((hd[3] & 3) << 11) | (hd[4] << 3) | (hd[5] >> 5);
        if (hd[0] != 0xFF || (hd[1] & 0xF6) != 0xF0 || flen < 7) {
            if (++bad > 4096) return -1;
            if (fseek(fp, -6, SEEK_CUR) != 0) return -1;
            continue;
        }
        if (fseek(fp, flen - 7, SEEK_CUR) != 0) return -1;
        i++;
    }
    return 0;
}

static int feed_file(resolved_t *r, int start_ms, sess_t *s, uint32_t gen) {
    char path[96];
    store_dl_path(r->id, "aac", path, sizeof path);
    FILE *fp = fopen(path, "rb");
    if (!fp) return FEED_FAIL_EARLY;
    setvbuf(fp, NULL, _IOFBF, 32 * 1024);
    int skip_ms = 0;
    if (start_ms > 0 && r->duration_ms > 0 && r->frames > 0) {
        long long fr = (long long)start_ms * r->frames / r->duration_ms;
        int skip_frames = fr > 2 ? (int)(fr - 2) : 0;
        if (skip_frames > 0) {
            if (adts_skip(fp, skip_frames) != 0) {
                fclose(fp);
                return FEED_FAIL_EARLY;
            }
            long long got = (long long)skip_frames * r->duration_ms / r->frames;
            skip_ms = start_ms - (int)got;
        } else {
            skip_ms = start_ms;
        }
    }
    feed_t *f = feed_new(s, gen, skip_ms);
    if (!f) {
        fclose(fp);
        return FEED_FAIL_EARLY;
    }
    aac_t *a = aac_new(pcm_cb, f);
    uint8_t *buf = malloc(16384);
    if (!a || !buf) {
        aac_free(a);
        free(buf);
        feed_free(f);
        fclose(fp);
        return FEED_FAIL_EARLY;
    }
    int result = FEED_OK;
    for (;;) {
        if (s->cancel) {
            result = FEED_CANCEL;
            break;
        }
        size_t n = fread(buf, 1, 16384, fp);
        if (n == 0) break;
        if (aac_push_adts(a, buf, n) < 0 && !f->failed) f->failed = 2;
        if (f->failed == 1) {
            result = FEED_CANCEL;
            break;
        }
        if (f->failed) {
            result = f->produced > 0 ? FEED_FAIL : FEED_FAIL_EARLY;
            break;
        }
    }
    if (result == FEED_OK && f->produced == 0) result = FEED_FAIL_EARLY;
    aac_free(a);
    free(buf);
    feed_free(f);
    fclose(fp);
    return result;
}

static void res_free(resolved_t *r) {
    hls_media_free(&r->hls);
    memset(r, 0, sizeof *r);
}

static int resolve_hls(http_t *h, const track_t *t, resolved_t *r, sess_t *s, char *err, size_t ec) {
    hls_media_t m;
    int rc = hlsres_resolve(h, t->id, &m, &s->cancel, err, ec);
    if (rc < 0) return rc;
    r->hls = m;
    r->kind = RES_HLS;
    r->duration_ms = (int)(m.total * 1000.0);
    str_set(r->id, sizeof r->id, t->id);
    return 0;
}

static int resolve_local(const track_t *t, resolved_t *r) {
    int frames = 0;
    int dur = 0;
    if (!store_dl_find(t->id, &frames, &dur)) return -1;
    char path[96];
    store_dl_path(t->id, "aac", path, sizeof path);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        log_msg("download missing for %s, dropping it from the index", t->id);
        store_dl_remove(t->id);
        return -1;
    }
    fclose(fp);
    r->kind = RES_FILE;
    r->frames = frames;
    r->duration_ms = dur;
    str_set(r->id, sizeof r->id, t->id);
    return 0;
}

static int resolve_track(http_t *h, const track_t *t, resolved_t *r, sess_t *s, char *err, size_t ec) {
    memset(r, 0, sizeof *r);
    err[0] = '\0';
    if (resolve_local(t, r) == 0) return 0;
    int rc = resolve_hls(h, t, r, s, err, ec);
    if (rc == 0) return 0;
    res_free(r);
    if (!s->cancel) log_msg("hls resolve failed for %s: %s", t->id, err);
    return -1;
}

static void request_play_locked(int idx, int start_ms) {
    P.req_idx = idx;
    P.req_start = start_ms;
    P.req_pending = 1;
    if (P.sess) P.sess->cancel = 1;
}

static volatile int g_radio = 1;

void player_set_radio(int on) {
    g_radio = on;
}

static void maybe_radio(int idx) {
    if (!g_radio) return;
    char last[16] = "";
    mx_lock(&P.mx);
    if (P.qn > 0 && P.qn - idx <= 3) str_set(last, sizeof last, P.q[P.qn - 1].id);
    mx_unlock(&P.mx);
    if (last[0]) jobs_radio(last);
}

static void wait_and_advance(http_t *h, int idx, sess_t *s, uint32_t gen, resolved_t *pre) {
    char err[160];
    int tried = 0;
    maybe_radio(idx);
    while (!s->cancel) {
        if (ring_finished(gen)) break;
        mx_lock(&P.mx);
        int has_next = idx + 1 < P.qn;
        track_t nt;
        if (has_next) nt = P.q[idx + 1];
        mx_unlock(&P.mx);
        if (has_next && !tried) {
            tried = 1;
            res_free(pre);
            if (resolve_track(h, &nt, pre, s, err, sizeof err) < 0) res_free(pre);
            continue;
        }
        sleep_ms(20);
    }
    for (int w = 0; w < 400 && !s->cancel; w++) {
        mx_lock(&P.mx);
        int has_next = idx + 1 < P.qn;
        if (has_next) request_play_locked(idx + 1, 0);
        mx_unlock(&P.mx);
        if (has_next) return;
        sleep_ms(20);
    }
    if (!s->cancel) {
        mx_lock(&P.mx);
        P.state = PL_IDLE;
        mx_unlock(&P.mx);
    }
}

static void fail_track(int idx, sess_t *s, const char *msg) {
    mx_lock(&P.mx);
    P.state = PL_ERROR;
    snprintf(P.msg, sizeof P.msg, "%s", msg);
    P.fail_streak++;
    int streak = P.fail_streak;
    int has_next = idx + 1 < P.qn;
    mx_unlock(&P.mx);
    if (streak < 3 && has_next) {
        for (int i = 0; i < 160 && !s->cancel; i++) sleep_ms(25);
        if (!s->cancel) {
            mx_lock(&P.mx);
            request_play_locked(idx + 1, 0);
            mx_unlock(&P.mx);
        }
    }
}

static void run_track(http_t *h, const track_t *t, int idx, int start_ms, sess_t *s, resolved_t *cur,
                      resolved_t *pre) {
    uint32_t gen = ++g_gen;
    ring_begin(gen, start_ms);
    char err[160] = "";

    int have = cur->kind != RES_NONE && strcmp(cur->id, t->id) == 0;
    if (!have) {
        if (pre->kind != RES_NONE && strcmp(pre->id, t->id) == 0) {
            res_free(cur);
            *cur = *pre;
            memset(pre, 0, sizeof *pre);
        } else {
            res_free(cur);
            if (resolve_track(h, t, cur, s, err, sizeof err) < 0) {
                if (!s->cancel) fail_track(idx, s, err[0] ? err : "Could not load this track");
                return;
            }
        }
    }
    if (s->cancel) return;

    mx_lock(&P.mx);
    P.duration_ms = cur->duration_ms;
    P.msg[0] = '\0';
    mx_unlock(&P.mx);

    if (start_ms == 0) store_hist_push(t);
    maybe_radio(idx);
    char art[300];
    art[0] = '\0';
    if (cur->kind == RES_FILE) {
        store_dl_path(t->id, "jpg", art, sizeof art);
        FILE *af = fopen(art, "rb");
        if (af) fclose(af);
        else art[0] = '\0';
    }
    if (!art[0]) {
        if (t->thumb[0]) snprintf(art, sizeof art, "%s", t->thumb);
        else snprintf(art, sizeof art, "https://i.ytimg.com/vi/%s/hqdefault.jpg", t->id);
    }
    jobs_art(art, t->id);

    int rc = cur->kind == RES_FILE ? feed_file(cur, start_ms, s, gen) : feed_hls(h, cur, start_ms, s, gen);

    if (s->cancel || rc == FEED_CANCEL) return;
    if (rc == FEED_FAIL_EARLY) {
        res_free(cur);
        fail_track(idx, s, err[0] ? err : "Playback failed");
        return;
    }
    if (rc == FEED_FAIL) log_msg("playback ended with a stream error");
    mx_lock(&P.mx);
    P.fail_streak = 0;
    mx_unlock(&P.mx);
    ring_set_eof(gen);
    wait_and_advance(h, idx, s, gen, pre);
}

static int player_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    http_t *h = http_new();
    if (!h) {
        log_msg("player: http_new failed");
        return 0;
    }
    resolved_t *cur = calloc(1, sizeof *cur);
    resolved_t *pre = calloc(1, sizeof *pre);
    while (!P.quit) {
        mx_lock(&P.mx);
        if (!P.req_pending) {
            mx_unlock(&P.mx);
            sleep_ms(15);
            continue;
        }
        P.req_pending = 0;
        int idx = P.req_idx;
        int start = P.req_start;
        if (idx < 0 || idx >= P.qn) {
            mx_unlock(&P.mx);
            continue;
        }
        sess_t *s = calloc(1, sizeof *s);
        P.sess = s;
        P.cur = idx;
        track_t t = P.q[idx];
        P.state = PL_LOADING;
        P.msg[0] = '\0';
        P.duration_ms = t.duration * 1000;
        mx_unlock(&P.mx);

        log_msg("play: [%d] %s - %s (%s) from %d ms", idx, t.artist, t.title, t.id, start);
        run_track(h, &t, idx, start, s, cur, pre);

        mx_lock(&P.mx);
        if (P.sess == s) P.sess = NULL;
        mx_unlock(&P.mx);
        free(s);
    }
    http_free(h);
    return 0;
}

static SceUID spawn_thread(const char *name, SceKernelThreadEntry fn, int prio, unsigned stack) {
    const int prios[] = {prio, 0x10000100};
    const unsigned stacks[] = {stack, 0x40000};
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            SceUID t = sceKernelCreateThread(name, fn, prios[i], stacks[j], 0, 0, NULL);
            if (t >= 0) {
                int r = sceKernelStartThread(t, 0, NULL);
                if (r >= 0) {
                    log_msg("thread %s started (prio %08x, stack %x)", name, (unsigned)prios[i], stacks[j]);
                    return t;
                }
                log_msg("thread %s start failed %08x", name, (unsigned)r);
                sceKernelDeleteThread(t);
            } else {
                log_msg("thread %s create failed %08x (prio %08x, stack %x)", name, (unsigned)t, (unsigned)prios[i],
                        stacks[j]);
            }
        }
    }
    return -1;
}

int player_init(void) {
    mx_init(&R.mx, "ytm_ring");
    mx_init(&P.mx, "ytm_player");
    log_msg("player mutexes %d %d", R.mx.id, P.mx.id);
    R.buf = malloc((size_t)RING_FRAMES * 4);
    P.q = malloc(sizeof(track_t) * QUEUE_CAP);
    if (!R.buf || !P.q) {
        log_msg("player: out of memory (ring %p, queue %p)", (void *)R.buf, (void *)P.q);
        return -1;
    }
    P.cur = -1;
    if (spawn_thread("ytm_audio", audio_thread, 96, 0x10000) < 0) return -1;
    if (spawn_thread("ytm_player", player_thread, 0x10000100, 0x80000) < 0) return -1;
    return 0;
}

void player_shutdown(void) {
    P.quit = 1;
    sleep_ms(100);
}

void player_queue_set(const track_t *t, int n, int start) {
    if (n <= 0) return;
    if (n > QUEUE_CAP) n = QUEUE_CAP;
    if (start < 0 || start >= n) start = 0;
    mx_lock(&P.mx);
    memcpy(P.q, t, sizeof(track_t) * (size_t)n);
    P.qn = n;
    P.state = PL_LOADING;
    P.fail_streak = 0;
    request_play_locked(start, 0);
    mx_unlock(&P.mx);
    mx_lock(&R.mx);
    R.paused = 0;
    mx_unlock(&R.mx);
}

static int queue_has_locked(const char *id) {
    for (int i = 0; i < P.qn; i++)
        if (strcmp(P.q[i].id, id) == 0) return 1;
    return 0;
}

int player_queue_add(const track_t *t) {
    int added = 0;
    mx_lock(&P.mx);
    if (P.qn < QUEUE_CAP && !queue_has_locked(t->id)) {
        P.q[P.qn++] = *t;
        added = 1;
    }
    int empty = P.sess == NULL && P.state == PL_IDLE && P.qn == 1;
    if (empty) request_play_locked(0, 0);
    mx_unlock(&P.mx);
    return added;
}

void player_queue_append(const track_t *t, int n) {
    mx_lock(&P.mx);
    int added = 0;
    for (int i = 0; i < n && P.qn < QUEUE_CAP; i++) {
        if (!queue_has_locked(t[i].id)) {
            P.q[P.qn++] = t[i];
            added++;
        }
    }
    mx_unlock(&P.mx);
    log_msg("queue: appended %d tracks", added);
}

static int queue_base_locked(void) {
    return P.req_pending ? P.req_idx : P.cur;
}

int player_queue_cur_index(void) {
    mx_lock(&P.mx);
    int base = queue_base_locked();
    mx_unlock(&P.mx);
    return base;
}

int player_queue_remove(int idx) {
    int ok = 0;
    mx_lock(&P.mx);
    if (idx > queue_base_locked() && idx < P.qn) {
        memmove(&P.q[idx], &P.q[idx + 1], sizeof(track_t) * (size_t)(P.qn - idx - 1));
        P.qn--;
        ok = 1;
    }
    mx_unlock(&P.mx);
    return ok;
}

int player_queue_trim(void) {
    int removed = 0;
    mx_lock(&P.mx);
    int base = queue_base_locked();
    if (base >= 0 && base + 1 < P.qn) {
        removed = P.qn - (base + 1);
        P.qn = base + 1;
    }
    mx_unlock(&P.mx);
    return removed;
}

int player_queue_count(void) {
    mx_lock(&P.mx);
    int n = P.qn;
    mx_unlock(&P.mx);
    return n;
}

int player_queue_get(int i, track_t *t) {
    int ok = 0;
    mx_lock(&P.mx);
    if (i >= 0 && i < P.qn) {
        *t = P.q[i];
        ok = 1;
    }
    mx_unlock(&P.mx);
    return ok;
}

int player_current(track_t *t) {
    int ok = 0;
    mx_lock(&P.mx);
    if (P.cur >= 0 && P.cur < P.qn) {
        *t = P.q[P.cur];
        ok = 1;
    }
    mx_unlock(&P.mx);
    return ok;
}

void player_play_index(int idx) {
    mx_lock(&P.mx);
    if (idx >= 0 && idx < P.qn) {
        P.state = PL_LOADING;
        P.fail_streak = 0;
        request_play_locked(idx, 0);
    }
    mx_unlock(&P.mx);
    mx_lock(&R.mx);
    R.paused = 0;
    mx_unlock(&R.mx);
}

void player_next(void) {
    mx_lock(&P.mx);
    int n = P.cur + 1;
    int ok = n < P.qn;
    if (ok) {
        P.state = PL_LOADING;
        P.fail_streak = 0;
        request_play_locked(n, 0);
    }
    mx_unlock(&P.mx);
    if (ok) {
        mx_lock(&R.mx);
        R.paused = 0;
        mx_unlock(&R.mx);
    }
}

void player_prev(void) {
    int pos = player_position_ms();
    mx_lock(&P.mx);
    int target = pos > 3000 ? P.cur : P.cur - 1;
    if (target < 0) target = 0;
    int ok = target < P.qn && P.qn > 0;
    if (ok) {
        P.state = PL_LOADING;
        P.fail_streak = 0;
        request_play_locked(target, 0);
    }
    mx_unlock(&P.mx);
    if (ok) {
        mx_lock(&R.mx);
        R.paused = 0;
        mx_unlock(&R.mx);
    }
}

void player_toggle_pause(void) {
    mx_lock(&R.mx);
    R.paused = !R.paused;
    int paused = R.paused;
    mx_unlock(&R.mx);
    mx_lock(&P.mx);
    if (P.state == PL_PLAYING && paused) P.state = PL_PAUSED;
    else if (P.state == PL_PAUSED && !paused) P.state = PL_PLAYING;
    mx_unlock(&P.mx);
}

void player_seek_abs(int ms) {
    mx_lock(&P.mx);
    int dur = P.duration_ms;
    if (P.cur >= 0 && P.cur < P.qn && (P.state == PL_PLAYING || P.state == PL_PAUSED)) {
        if (ms < 0) ms = 0;
        if (dur > 1500 && ms > dur - 1500) ms = dur - 1500;
        request_play_locked(P.cur, ms);
    }
    mx_unlock(&P.mx);
}

void player_seek_rel(int ms) {
    player_seek_abs(player_position_ms() + ms);
}

pl_state_t player_state(void) {
    mx_lock(&P.mx);
    pl_state_t s = P.state;
    mx_unlock(&P.mx);
    return s;
}

int player_position_ms(void) {
    mx_lock(&R.mx);
    int rate = OUT_RATE;
    int ms = R.base_ms + (int)((R.played * 1000ull) / (uint64_t)rate);
    mx_unlock(&R.mx);
    return ms;
}

int player_duration_ms(void) {
    mx_lock(&P.mx);
    int d = P.duration_ms;
    mx_unlock(&P.mx);
    return d;
}

void player_message(char *out, size_t cap) {
    mx_lock(&P.mx);
    str_set(out, cap, P.msg);
    mx_unlock(&P.mx);
}
