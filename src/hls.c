#include "hls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *resolve_url(const char *base, const char *ref) {
    if (strstr(ref, "://")) return strdup(ref);
    size_t rl = strlen(ref);
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end) return strdup(ref);
    if (ref[0] == '/') {
        const char *host_end = strchr(scheme_end + 3, '/');
        size_t hl = host_end ? (size_t)(host_end - base) : strlen(base);
        char *r = malloc(hl + rl + 1);
        if (!r) return NULL;
        memcpy(r, base, hl);
        memcpy(r + hl, ref, rl + 1);
        return r;
    }
    size_t blen = strlen(base);
    const char *q = strpbrk(base, "?#");
    if (q) blen = (size_t)(q - base);
    while (blen > 0 && base[blen - 1] != '/') blen--;
    char *r = malloc(blen + rl + 1);
    if (!r) return NULL;
    memcpy(r, base, blen);
    memcpy(r + blen, ref, rl + 1);
    return r;
}

static int itag_of(const char *uri) {
    const char *p = strstr(uri, "/itag/");
    if (p) return atoi(p + 6);
    p = strstr(uri, "itag=");
    if (p) return atoi(p + 5);
    return 0;
}

static int attr_value(const char *line, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == line || p[-1] == ',' || p[-1] == ':' || p[-1] == ' ') && p[kl] == '=') {
            p += kl + 1;
            size_t n = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
            } else {
                while (*p && *p != ',' && n + 1 < cap) out[n++] = *p++;
            }
            out[n] = '\0';
            return 1;
        }
        p += kl;
    }
    return 0;
}

static int cmp_cand(const void *a, const void *b) {
    const hls_cand_t *x = a;
    const hls_cand_t *y = b;
    return y->prio - x->prio;
}

int hls_pick_variants(const char *master, const char *base, hls_cand_t *out, int max) {
    char *copy = strdup(master);
    if (!copy) return 0;
    int n = 0;
    char *save = NULL;
    int pending_bw = -1;
    for (char *line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (strncmp(line, "#EXT-X-MEDIA:", 13) == 0) {
            char type[32];
            char uri[2600];
            if (!attr_value(line, "TYPE", type, sizeof type) || strcmp(type, "AUDIO") != 0) continue;
            if (!attr_value(line, "URI", uri, sizeof uri)) continue;
            if (n >= max) continue;
            char *full = resolve_url(base, uri);
            if (!full) continue;
            int itag = itag_of(full);
            int prio = 500;
            if (itag == 234) prio = 1000;
            else if (itag == 233) prio = 900;
            else if (itag == 140) prio = 950;
            char name[96];
            char def[16];
            if (attr_value(line, "NAME", name, sizeof name) && strstr(name, "original")) prio += 60;
            if (attr_value(line, "DEFAULT", def, sizeof def) && strcmp(def, "YES") == 0) prio += 25;
            out[n].uri = full;
            out[n].prio = prio;
            n++;
        } else if (strncmp(line, "#EXT-X-STREAM-INF:", 18) == 0) {
            char bw[24];
            pending_bw = 0;
            if (attr_value(line, "BANDWIDTH", bw, sizeof bw)) pending_bw = atoi(bw);
        } else if (line[0] != '#' && pending_bw >= 0) {
            if (n < max) {
                char *full = resolve_url(base, line);
                if (full) {
                    int prio = 400 - pending_bw / 20000;
                    if (prio < 1) prio = 1;
                    out[n].uri = full;
                    out[n].prio = prio;
                    n++;
                }
            }
            pending_bw = -1;
        }
    }
    free(copy);
    qsort(out, (size_t)n, sizeof out[0], cmp_cand);
    return n;
}

void hls_cands_free(hls_cand_t *c, int n) {
    for (int i = 0; i < n; i++) {
        free(c[i].uri);
        c[i].uri = NULL;
    }
}

int hls_parse_media(char *text, const char *base, hls_media_t *out) {
    memset(out, 0, sizeof *out);
    out->text = text;
    int cap = 64;
    out->segs = calloc((size_t)cap, sizeof(hls_seg_t));
    if (!out->segs) return -1;
    double pending = -1;
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (strncmp(line, "#EXTINF:", 8) == 0) {
            pending = atof(line + 8);
        } else if (strncmp(line, "#EXT-X-MAP", 10) == 0) {
            out->has_map = 1;
        } else if (line[0] != '#' && line[0] != '\0') {
            if (out->n == cap) {
                cap *= 2;
                hls_seg_t *ns = realloc(out->segs, (size_t)cap * sizeof(hls_seg_t));
                if (!ns) return -1;
                out->segs = ns;
            }
            hls_seg_t *s = &out->segs[out->n];
            if (strstr(line, "://")) {
                s->uri = line;
                s->owned = 0;
            } else {
                s->uri = resolve_url(base, line);
                s->owned = 1;
                if (!s->uri) return -1;
            }
            s->dur = pending > 0 ? pending : 5.0;
            out->total += s->dur;
            out->n++;
            pending = -1;
        }
    }
    return out->n;
}

void hls_media_free(hls_media_t *m) {
    if (!m) return;
    if (m->segs) {
        for (int i = 0; i < m->n; i++)
            if (m->segs[i].owned) free(m->segs[i].uri);
        free(m->segs);
    }
    free(m->text);
    memset(m, 0, sizeof *m);
}

int hls_find_segment(const hls_media_t *m, int ms, int *seg_start_ms) {
    double acc = 0;
    double target = ms / 1000.0;
    for (int i = 0; i < m->n; i++) {
        if (target < acc + m->segs[i].dur || i == m->n - 1) {
            if (seg_start_ms) *seg_start_ms = (int)(acc * 1000.0);
            return i;
        }
        acc += m->segs[i].dur;
    }
    if (seg_start_ms) *seg_start_ms = 0;
    return 0;
}
