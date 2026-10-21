#include "ytm.h"

#include <ctype.h>
#include <jansson.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/rtc.h>

#define UA_WEB "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:88.0) Gecko/20100101 Firefox/88.0"
#define SEARCH_PARAMS_SONGS "EgWKAQIIAWoMEA4QChADEAQQCRAF"
#define VISION_VERSION "1.02"
#define MIN_WEB_DATE 20260901

static char g_hl[16] = "en";
static char g_gl[16] = "US";
static char g_vd[2048];
static char g_vd_yt[2048];
static int g_vd_tries;
static mutex_t g_mx;
static int g_mx_ready;

typedef struct {
    const char *p[12];
    char s[8][2300];
    int n;
} hdrs_t;

static void hdr_add(hdrs_t *h, const char *fmt, ...) {
    if (h->n >= 11) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->s[h->n], sizeof h->s[0], fmt, ap);
    va_end(ap);
    h->p[h->n] = h->s[h->n];
    h->n++;
    h->p[h->n] = NULL;
}

static void hdr_init(hdrs_t *h) {
    memset(h, 0, sizeof *h);
}

static json_t *jp(json_t *n, ...) {
    va_list ap;
    va_start(ap, n);
    const char *k;
    while (n && (k = va_arg(ap, const char *))) {
        if (k[0] == '#')
            n = json_is_array(n) ? json_array_get(n, (size_t)atoi(k + 1)) : NULL;
        else
            n = json_is_object(n) ? json_object_get(n, k) : NULL;
    }
    va_end(ap);
    return n;
}

static const char *jstr(json_t *n) {
    return json_is_string(n) ? json_string_value(n) : NULL;
}

void ytm_configure(const char *hl, const char *gl) {
    if (!g_mx_ready) {
        mx_init(&g_mx, "ytm");
        g_mx_ready = 1;
    }
    if (hl && hl[0]) str_set(g_hl, sizeof g_hl, hl);
    if (gl && gl[0]) str_set(g_gl, sizeof g_gl, gl);
}

static void vd_get(char *out, size_t cap) {
    mx_lock(&g_mx);
    str_set(out, cap, g_vd);
    mx_unlock(&g_mx);
}

static void vd_set(const char *v, size_t len) {
    if (!len || len >= sizeof g_vd) return;
    mx_lock(&g_mx);
    memcpy(g_vd, v, len);
    g_vd[len] = '\0';
    mx_unlock(&g_mx);
}

static void vd_yt_get(char *out, size_t cap) {
    mx_lock(&g_mx);
    str_set(out, cap, g_vd_yt);
    mx_unlock(&g_mx);
}

static void vd_yt_set(const char *v, size_t len) {
    if (!len || len >= sizeof g_vd_yt) return;
    mx_lock(&g_mx);
    memcpy(g_vd_yt, v, len);
    g_vd_yt[len] = '\0';
    mx_unlock(&g_mx);
}

static void harvest_visitor(const char *body, int yt) {
    static const char key[] = "\"visitorData\":\"";
    if (!body) return;
    const char *k = strstr(body, key);
    if (!k) return;
    const char *v = k + (sizeof key - 1);
    const char *e = strchr(v, '"');
    if (!e || e == v) return;
    if (yt) vd_yt_set(v, (size_t)(e - v));
    else vd_set(v, (size_t)(e - v));
}

typedef struct {
    buf_t b;
    size_t scanned;
    char vd[2048];
    int found;
} vsink_t;

static int vis_sink(const uint8_t *d, size_t n, void *u) {
    vsink_t *s = u;
    if (buf_append(&s->b, d, n) < 0) return -1;
    if (s->b.len > 3u * 1024u * 1024u) return -1;
    char *k = strstr(s->b.data + s->scanned, "VISITOR_DATA");
    if (!k) {
        s->scanned = s->b.len > 16 ? s->b.len - 16 : 0;
        return 0;
    }
    char *q = k + 12;
    while (*q == '"' || *q == '\\' || *q == ':' || *q == ' ') q++;
    char *t = q;
    while (isalnum((unsigned char)*q) || *q == '-' || *q == '_' || *q == '%' || *q == '=') q++;
    if (!*q) {
        s->scanned = (size_t)(k - s->b.data);
        return 0;
    }
    size_t len = (size_t)(q - t);
    if (len >= 8 && len < sizeof s->vd) {
        memcpy(s->vd, t, len);
        s->vd[len] = '\0';
        s->found = 1;
        return 1;
    }
    s->scanned = (size_t)(q - s->b.data);
    return 0;
}

static void ensure_visitor(http_t *h, volatile int *cancel) {
    char cur[8];
    vd_get(cur, sizeof cur);
    if (cur[0]) return;
    mx_lock(&g_mx);
    int tries = g_vd_tries++;
    mx_unlock(&g_mx);
    if (tries >= 2) return;
    hdrs_t hd;
    hdr_init(&hd);
    hdr_add(&hd, "User-Agent: " UA_WEB);
    hdr_add(&hd, "Cookie: SOCS=CAI");
    hdr_add(&hd, "Accept-Language: en-US,en;q=0.5");
    vsink_t s;
    memset(&s, 0, sizeof s);
    buf_init(&s.b);
    http_req_t r;
    memset(&r, 0, sizeof r);
    r.url = "https://music.youtube.com/";
    r.headers = hd.p;
    r.range_start = -1;
    r.range_end = -1;
    r.timeout_s = 30;
    r.compress = 1;
    r.cancel = cancel;
    r.sink = vis_sink;
    r.user = &s;
    long status = 0;
    http_run(h, &r, &status);
    if (s.found) {
        vd_set(s.vd, strlen(s.vd));
        log_msg("visitor id from page (%zu chars)", strlen(s.vd));
    } else {
        log_msg("visitor id not found in page (status %ld)", status);
    }
    buf_free(&s.b);
}

static void web_version(char *out, size_t cap) {
    SceDateTime dt;
    int ymd = 0;
    if (sceRtcGetCurrentClock(&dt, 0) >= 0) ymd = dt.year * 10000 + dt.month * 100 + dt.day;
    if (ymd < MIN_WEB_DATE) ymd = MIN_WEB_DATE;
    snprintf(out, cap, "1.%08d.01.00", ymd);
}

static json_t *context_web_remix(void) {
    char ver[32];
    char vd[2048];
    web_version(ver, sizeof ver);
    vd_get(vd, sizeof vd);
    json_t *client = json_object();
    json_object_set_new(client, "clientName", json_string("WEB_REMIX"));
    json_object_set_new(client, "clientVersion", json_string(ver));
    json_object_set_new(client, "hl", json_string(g_hl));
    json_object_set_new(client, "gl", json_string(g_gl));
    if (vd[0]) json_object_set_new(client, "visitorData", json_string(vd));
    json_t *ctx = json_object();
    json_object_set_new(ctx, "client", client);
    json_object_set_new(ctx, "user", json_object());
    return ctx;
}

static json_t *context_visionos(void) {
    return json_pack("{s:{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:i}}", "client", "clientName", "VISIONOS",
                     "clientVersion", VISION_VERSION, "deviceMake", "Apple", "deviceModel", "RealityDevice17,1",
                     "userAgent", YTM_UA_VISION, "osName", "visionOS", "osVersion", "26.5.23O471", "hl", g_hl, "gl",
                     g_gl, "timeZone", "UTC", "utcOffsetMinutes", 0);
}

static int post_json(http_t *h, const char *url, json_t *body, hdrs_t *hd, volatile int *cancel, buf_t *out,
                     long *status, int yt) {
    char *payload = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    if (!payload) return HTTP_FAILED;
    http_req_t r;
    memset(&r, 0, sizeof r);
    r.url = url;
    r.post = payload;
    r.post_len = strlen(payload);
    r.headers = hd->p;
    r.range_start = -1;
    r.range_end = -1;
    r.timeout_s = 40;
    r.compress = 1;
    r.cancel = cancel;
    int rc = http_fetch(h, &r, out, 0, status);
    free(payload);
    if (out->data) harvest_visitor(out->data, yt);
    return rc;
}

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static int sep_run(const char *t) {
    return strstr(t, "\xE2\x80\xA2") != NULL && strlen(t) <= 6;
}

static void find_thumb(json_t *n, char *out, size_t cap, int depth) {
    if (out[0] || depth > 12) return;
    if (json_is_object(n)) {
        json_t *th = json_object_get(n, "thumbnails");
        if (json_is_array(th) && json_array_size(th) > 0) {
            json_t *last = json_array_get(th, json_array_size(th) - 1);
            const char *u = jstr(json_object_get(last, "url"));
            if (u) {
                str_set(out, cap, u);
                if (strstr(out, "googleusercontent.com") || strstr(out, "ggpht.com")) {
                    char *eq = strrchr(out, '=');
                    if (eq && (size_t)(eq - out) + 24 < cap) strcpy(eq, "=w256-h256-l90-rj");
                }
                return;
            }
        }
        const char *k;
        json_t *v;
        json_object_foreach(n, k, v) {
            find_thumb(v, out, cap, depth + 1);
            if (out[0]) return;
        }
    } else if (json_is_array(n)) {
        size_t i;
        json_t *v;
        json_array_foreach(n, i, v) {
            find_thumb(v, out, cap, depth + 1);
            if (out[0]) return;
        }
    }
}

static void parse_byline(json_t *runs, track_t *t) {
    char seg[160] = "";
    char artist[96] = "";
    char album[96] = "";
    char unlinked[2][96] = {"", ""};
    int n_unlinked = 0;
    int has_artist = 0;
    int has_album = 0;
    int dur = -1;
    size_t cnt = json_is_array(runs) ? json_array_size(runs) : 0;
    for (size_t i = 0; i <= cnt; i++) {
        const char *text = NULL;
        json_t *run = NULL;
        int flush = 0;
        if (i == cnt) {
            flush = 1;
        } else {
            run = json_array_get(runs, i);
            text = jstr(json_object_get(run, "text"));
            if (!text) continue;
            if (sep_run(text)) flush = 1;
        }
        if (flush) {
            if (seg[0]) {
                int d = parse_duration(seg);
                if (d >= 0) {
                    dur = d;
                } else if (has_artist && !artist[0]) {
                    str_set(artist, sizeof artist, seg);
                } else if (has_album && !album[0]) {
                    str_set(album, sizeof album, seg);
                } else if (!has_artist && !has_album && n_unlinked < 2) {
                    str_set(unlinked[n_unlinked++], sizeof unlinked[0], seg);
                }
            }
            seg[0] = '\0';
            has_artist = 0;
            has_album = 0;
            continue;
        }
        size_t sl = strlen(seg);
        if (sl + strlen(text) + 1 < sizeof seg) strcat(seg, text);
        const char *bid = jstr(jp(run, "navigationEndpoint", "browseEndpoint", "browseId", NULL));
        if (bid) {
            if (starts_with(bid, "UC") || starts_with(bid, "MPLA"))
                has_artist = 1;
            else if (starts_with(bid, "MPRE"))
                has_album = 1;
        }
    }
    if (!artist[0]) {
        if (n_unlinked >= 2)
            str_set(artist, sizeof artist, unlinked[1]);
        else if (n_unlinked == 1)
            str_set(artist, sizeof artist, unlinked[0]);
    }
    str_set(t->artist, sizeof t->artist, artist);
    str_set(t->album, sizeof t->album, album);
    if (dur >= 0) t->duration = dur;
}

static int parse_list_item(json_t *r, track_t *t) {
    memset(t, 0, sizeof *t);
    const char *vid = jstr(jp(r, "playlistItemData", "videoId", NULL));
    if (!vid)
        vid = jstr(jp(r, "overlay", "musicItemThumbnailOverlayRenderer", "content", "musicPlayButtonRenderer",
                      "playNavigationEndpoint", "watchEndpoint", "videoId", NULL));
    if (!vid || strlen(vid) != 11) return 0;
    const char *pol = jstr(json_object_get(r, "musicItemRendererDisplayPolicy"));
    if (pol && strstr(pol, "GREY_OUT")) return 0;
    const char *title = jstr(jp(r, "flexColumns", "#0", "musicResponsiveListItemFlexColumnRenderer", "text", "runs",
                                "#0", "text", NULL));
    if (!title) return 0;
    str_set(t->id, sizeof t->id, vid);
    str_set(t->title, sizeof t->title, title);
    parse_byline(jp(r, "flexColumns", "#1", "musicResponsiveListItemFlexColumnRenderer", "text", "runs", NULL), t);
    find_thumb(r, t->thumb, sizeof t->thumb, 0);
    return 1;
}

static int parse_panel_item(json_t *r, track_t *t) {
    memset(t, 0, sizeof *t);
    const char *vid = jstr(json_object_get(r, "videoId"));
    if (!vid || strlen(vid) != 11) return 0;
    if (json_object_get(r, "unplayableText")) return 0;
    const char *title = jstr(jp(r, "title", "runs", "#0", "text", NULL));
    if (!title) return 0;
    str_set(t->id, sizeof t->id, vid);
    str_set(t->title, sizeof t->title, title);
    parse_byline(jp(r, "longBylineText", "runs", NULL), t);
    const char *len = jstr(jp(r, "lengthText", "runs", "#0", "text", NULL));
    if (len) {
        int d = parse_duration(len);
        if (d >= 0) t->duration = d;
    }
    find_thumb(json_object_get(r, "thumbnail"), t->thumb, sizeof t->thumb, 0);
    return 1;
}

typedef struct {
    track_t *out;
    int n;
    int max;
    int mode;
} coll_t;

static int coll_add(coll_t *c, const track_t *t) {
    for (int i = 0; i < c->n; i++)
        if (strcmp(c->out[i].id, t->id) == 0) return 0;
    c->out[c->n++] = *t;
    return 1;
}

static void collect(json_t *n, coll_t *c, int depth) {
    if (c->n >= c->max || depth > 48) return;
    track_t t;
    if (json_is_object(n)) {
        const char *k;
        json_t *v;
        json_object_foreach(n, k, v) {
            if (c->mode == 0 && strcmp(k, "musicResponsiveListItemRenderer") == 0) {
                if (parse_list_item(v, &t)) coll_add(c, &t);
            } else if (c->mode == 1 && strcmp(k, "playlistPanelVideoRenderer") == 0) {
                if (parse_panel_item(v, &t)) coll_add(c, &t);
            } else if (c->mode == 1 && strcmp(k, "playlistPanelVideoWrapperRenderer") == 0) {
                json_t *pr = jp(v, "primaryRenderer", "playlistPanelVideoRenderer", NULL);
                if (pr && parse_panel_item(pr, &t)) coll_add(c, &t);
            } else {
                collect(v, c, depth + 1);
            }
            if (c->n >= c->max) return;
        }
    } else if (json_is_array(n)) {
        size_t i;
        json_t *v;
        json_array_foreach(n, i, v) {
            collect(v, c, depth + 1);
            if (c->n >= c->max) return;
        }
    }
}

static void web_headers(hdrs_t *hd) {
    char vd[2048];
    vd_get(vd, sizeof vd);
    hdr_init(hd);
    hdr_add(hd, "Content-Type: application/json");
    hdr_add(hd, "Accept: */*");
    hdr_add(hd, "User-Agent: " UA_WEB);
    hdr_add(hd, "Origin: https://music.youtube.com");
    if (vd[0]) hdr_add(hd, "X-Goog-Visitor-Id: %s", vd);
}

static int music_call(http_t *h, const char *endpoint, json_t *body, volatile int *cancel, json_t **root, char *err,
                      size_t errcap) {
    ensure_visitor(h, cancel);
    json_object_set_new(body, "context", context_web_remix());
    hdrs_t hd;
    web_headers(&hd);
    char url[160];
    snprintf(url, sizeof url, "https://music.youtube.com/youtubei/v1/%s?prettyPrint=false", endpoint);
    buf_t out;
    buf_init(&out);
    long status = 0;
    int rc = post_json(h, url, body, &hd, cancel, &out, &status, 0);
    if (rc == HTTP_CANCELLED) {
        buf_free(&out);
        return -1;
    }
    if (rc != HTTP_OK) {
        set_err(err, errcap, "Network error: %s", http_last_error(h));
        buf_free(&out);
        return -2;
    }
    if (status != 200) {
        log_msg("%s: http %ld", endpoint, status);
        set_err(err, errcap, "YouTube Music returned HTTP %ld", status);
        buf_free(&out);
        return -3;
    }
    json_error_t je;
    *root = json_loadb(out.data, out.len, 0, &je);
    buf_free(&out);
    if (!*root) {
        log_msg("%s: bad json: %s", endpoint, je.text);
        set_err(err, errcap, "Unreadable response");
        return -4;
    }
    return 0;
}

static int do_search(http_t *h, const char *query, const char *params, track_t *out, int max, volatile int *cancel,
                     char *err, size_t errcap) {
    json_t *body = json_object();
    json_object_set_new(body, "query", json_string(query));
    if (params) json_object_set_new(body, "params", json_string(params));
    json_t *root = NULL;
    int rc = music_call(h, "search", body, cancel, &root, err, errcap);
    if (rc < 0) return rc;
    coll_t c = {out, 0, max, 0};
    collect(root, &c, 0);
    json_decref(root);
    return c.n;
}

int ytm_search(http_t *h, const char *query, track_t *out, int max, volatile int *cancel, char *err, size_t errcap) {
    int n = do_search(h, query, SEARCH_PARAMS_SONGS, out, max, cancel, err, errcap);
    log_msg("search '%s': %d songs", query, n);
    if (n == 0) {
        n = do_search(h, query, NULL, out, max, cancel, err, errcap);
        log_msg("search '%s' unfiltered: %d", query, n);
    }
    if (n == 0) set_err(err, errcap, "No results");
    return n;
}

int ytm_radio(http_t *h, const char *video_id, track_t *out, int max, volatile int *cancel, char *err, size_t errcap) {
    char pl[40];
    snprintf(pl, sizeof pl, "RDAMVM%s", video_id);
    json_t *body = json_pack(
        "{s:s,s:s,s:b,s:b,s:s,s:{s:{s:b,s:s}}}", "videoId", video_id, "playlistId", pl, "isAudioOnly", 1,
        "enablePersistentPlaylistPanel", 1, "tunerSettingValue", "AUTOMIX_SETTING_NORMAL",
        "watchEndpointMusicSupportedConfigs", "watchEndpointMusicConfig", "hasPersistentPlaylistPanel", 1,
        "musicVideoType", "MUSIC_VIDEO_TYPE_ATV");
    json_t *root = NULL;
    int rc = music_call(h, "next", body, cancel, &root, err, errcap);
    if (rc < 0) return rc;
    coll_t c = {out, 0, max, 1};
    collect(root, &c, 0);
    json_decref(root);
    log_msg("radio %s: %d tracks", video_id, c.n);
    return c.n;
}

static int player_call(http_t *h, const char *video_id, volatile int *cancel, json_t **root, char *err,
                       size_t errcap) {
    char sent_vd[2048] = "";
    for (int a = 0; a < 2; a++) {
        json_t *body = json_pack("{s:s,s:b,s:b}", "videoId", video_id, "contentCheckOk", 1, "racyCheckOk", 1);
        hdrs_t hd;
        hdr_init(&hd);
        hdr_add(&hd, "Content-Type: application/json");
        sent_vd[0] = '\0';
        json_object_set_new(body, "context", context_visionos());
        json_object_set_new(body, "playbackContext",
                            json_pack("{s:{s:s}}", "contentPlaybackContext", "html5Preference", "HTML5_PREF_WANTS"));
        hdr_add(&hd, "X-Youtube-Client-Name: 101");
        hdr_add(&hd, "X-Youtube-Client-Version: " VISION_VERSION);
        hdr_add(&hd, "User-Agent: " YTM_UA_VISION);
        hdr_add(&hd, "Origin: https://www.youtube.com");
        vd_yt_get(sent_vd, sizeof sent_vd);
        if (sent_vd[0]) hdr_add(&hd, "X-Goog-Visitor-Id: %s", sent_vd);
        const char *url = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
        buf_t out;
        buf_init(&out);
        long status = 0;
        int rc = post_json(h, url, body, &hd, cancel, &out, &status, 1);
        if (rc == HTTP_CANCELLED) {
            buf_free(&out);
            return -1;
        }
        if (rc != HTTP_OK) {
            set_err(err, errcap, "network: %s", http_last_error(h));
            buf_free(&out);
            return -2;
        }
        json_error_t je;
        json_t *r = out.data ? json_loadb(out.data, out.len, 0, &je) : NULL;
        buf_free(&out);
        if (!r) {
            set_err(err, errcap, "player HTTP %ld, unreadable reply", status);
            log_msg("player %s: http %ld bad json", video_id, status);
            return -4;
        }
        const char *st = jstr(jp(r, "playabilityStatus", "status", NULL));
        const char *reason = jstr(jp(r, "playabilityStatus", "reason", NULL));
        log_msg("player %s: http %ld playability=%s%s%s", video_id, status, st ? st : "?", reason ? " reason=" : "",
                reason ? reason : "");
        if (st && strcmp(st, "OK") == 0) {
            *root = r;
            return 0;
        }
        set_err(err, errcap, "%s: %s", st ? st : "no status", reason ? reason : "no reason given");
        json_decref(r);
        char now_vd[2048];
        vd_yt_get(now_vd, sizeof now_vd);
        if (!(a == 0 && now_vd[0] && strcmp(now_vd, sent_vd) != 0)) break;
        log_msg("player: retrying with new visitor id");
    }
    return -5;
}

int ytm_hls_master(http_t *h, const char *video_id, char *url, size_t cap, volatile int *cancel, char *err,
                   size_t errcap) {
    url[0] = '\0';
    json_t *root = NULL;
    int rc = player_call(h, video_id, cancel, &root, err, errcap);
    if (rc < 0) return rc;
    const char *m = jstr(jp(root, "streamingData", "hlsManifestUrl", NULL));
    if (m && m[0]) {
        str_set(url, cap, m);
        json_decref(root);
        return 0;
    }
    set_err(err, errcap, "No HLS manifest");
    json_decref(root);
    return -6;
}
