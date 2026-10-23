#include "store.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#define FAV_PATH DATA_DIR "/favorites.json"
#define HIST_PATH DATA_DIR "/history.json"
#define PL_PATH DATA_DIR "/playlists.json"
#define DL_PATH DATA_DIR "/downloads.json"
#define FAV_MAX 1000
#define HIST_MAX 100
#define PL_MAX 64
#define PL_TRACKS_MAX 500
#define PL_NAME_MAX 64
#define DL_MAX 1000

typedef struct {
    track_t *items;
    int n;
    int cap;
} list_t;

typedef struct {
    char name[PL_NAME_MAX];
    track_t *items;
    int n;
    int cap;
} playlist_t;

typedef struct {
    track_t t;
    int frames;
    int dur_ms;
} dl_item_t;

static list_t g_fav;
static list_t g_hist;
static playlist_t g_pl[PL_MAX];
static int g_pln;
static dl_item_t *g_dl;
static int g_dln;
static mutex_t g_mx;

static json_t *track_to_json(const track_t *t) {
    return json_pack("{s:s,s:s,s:s,s:s,s:s,s:i}", "id", t->id, "title", t->title, "artist", t->artist, "album",
                     t->album, "thumb", t->thumb, "dur", t->duration);
}

static void json_to_track(json_t *o, track_t *t) {
    memset(t, 0, sizeof *t);
    const char *s;
    s = json_string_value(json_object_get(o, "id"));
    str_set(t->id, sizeof t->id, s);
    s = json_string_value(json_object_get(o, "title"));
    str_set(t->title, sizeof t->title, s);
    s = json_string_value(json_object_get(o, "artist"));
    str_set(t->artist, sizeof t->artist, s);
    s = json_string_value(json_object_get(o, "album"));
    str_set(t->album, sizeof t->album, s);
    s = json_string_value(json_object_get(o, "thumb"));
    str_set(t->thumb, sizeof t->thumb, s);
    t->duration = (int)json_integer_value(json_object_get(o, "dur"));
}

static void list_save(const list_t *l, const char *path) {
    json_t *arr = json_array();
    for (int i = 0; i < l->n; i++) json_array_append_new(arr, track_to_json(&l->items[i]));
    json_dump_file(arr, path, JSON_COMPACT);
    json_decref(arr);
}

static void list_load(list_t *l, const char *path, int max) {
    l->items = calloc((size_t)max, sizeof(track_t));
    l->cap = max;
    l->n = 0;
    if (!l->items) return;
    json_error_t e;
    json_t *arr = json_load_file(path, 0, &e);
    if (!arr) return;
    size_t i;
    json_t *o;
    json_array_foreach(arr, i, o) {
        if (l->n >= max) break;
        json_to_track(o, &l->items[l->n]);
        if (strlen(l->items[l->n].id) == 11) l->n++;
    }
    json_decref(arr);
}

static int list_find(const list_t *l, const char *id) {
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->items[i].id, id) == 0) return i;
    return -1;
}

static void list_remove(list_t *l, int idx) {
    memmove(&l->items[idx], &l->items[idx + 1], (size_t)(l->n - idx - 1) * sizeof(track_t));
    l->n--;
}

static void list_push_front(list_t *l, const track_t *t) {
    if (l->n >= l->cap) l->n = l->cap - 1;
    memmove(&l->items[1], &l->items[0], (size_t)l->n * sizeof(track_t));
    l->items[0] = *t;
    l->n++;
}

static void utf8_fix(char *s) {
    size_t i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        size_t need = c < 0x80 ? 1 : ((c >> 5) == 6 ? 2 : ((c >> 4) == 14 ? 3 : ((c >> 3) == 30 ? 4 : 0)));
        if (need == 0) {
            s[i] = '\0';
            return;
        }
        for (size_t k = 1; k < need; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) {
                s[i] = '\0';
                return;
            }
        }
        i += need;
    }
}

static void pl_clear(playlist_t *p) {
    free(p->items);
    memset(p, 0, sizeof *p);
}

static int pl_reserve(playlist_t *p, int need) {
    if (need <= p->cap) return 1;
    int cap = p->cap ? p->cap * 2 : 16;
    while (cap < need) cap *= 2;
    if (cap > PL_TRACKS_MAX) cap = PL_TRACKS_MAX;
    if (cap < need) return 0;
    track_t *ni = realloc(p->items, sizeof(track_t) * (size_t)cap);
    if (!ni) return 0;
    p->items = ni;
    p->cap = cap;
    return 1;
}

static void pl_save(void) {
    json_t *arr = json_array();
    for (int i = 0; i < g_pln; i++) {
        json_t *tracks = json_array();
        for (int k = 0; k < g_pl[i].n; k++) json_array_append_new(tracks, track_to_json(&g_pl[i].items[k]));
        json_t *o = json_object();
        json_object_set_new(o, "name", json_string(g_pl[i].name));
        json_object_set_new(o, "tracks", tracks);
        json_array_append_new(arr, o);
    }
    json_dump_file(arr, PL_PATH, JSON_COMPACT);
    json_decref(arr);
}

static void pl_load(void) {
    json_error_t e;
    json_t *arr = json_load_file(PL_PATH, 0, &e);
    if (!arr) return;
    size_t i;
    json_t *o;
    json_array_foreach(arr, i, o) {
        if (g_pln >= PL_MAX) break;
        const char *name = json_string_value(json_object_get(o, "name"));
        if (!name || !name[0]) continue;
        playlist_t *p = &g_pl[g_pln];
        memset(p, 0, sizeof *p);
        str_set(p->name, sizeof p->name, name);
        utf8_fix(p->name);
        json_t *tracks = json_object_get(o, "tracks");
        size_t k;
        json_t *to;
        json_array_foreach(tracks, k, to) {
            if (p->n >= PL_TRACKS_MAX) break;
            track_t t;
            json_to_track(to, &t);
            if (strlen(t.id) != 11) continue;
            if (!pl_reserve(p, p->n + 1)) break;
            p->items[p->n++] = t;
        }
        g_pln++;
    }
    json_decref(arr);
}

static void dl_save(void) {
    json_t *arr = json_array();
    for (int i = 0; i < g_dln; i++) {
        json_t *o = track_to_json(&g_dl[i].t);
        if (!o) continue;
        json_object_set_new(o, "frames", json_integer(g_dl[i].frames));
        json_object_set_new(o, "dur_ms", json_integer(g_dl[i].dur_ms));
        json_array_append_new(arr, o);
    }
    json_dump_file(arr, DL_PATH, JSON_COMPACT);
    json_decref(arr);
}

static void dl_load(void) {
    g_dl = calloc(DL_MAX, sizeof(dl_item_t));
    g_dln = 0;
    if (!g_dl) return;
    json_error_t e;
    json_t *arr = json_load_file(DL_PATH, 0, &e);
    if (!arr) return;
    size_t i;
    json_t *o;
    json_array_foreach(arr, i, o) {
        if (g_dln >= DL_MAX) break;
        dl_item_t it;
        memset(&it, 0, sizeof it);
        json_to_track(o, &it.t);
        it.frames = (int)json_integer_value(json_object_get(o, "frames"));
        it.dur_ms = (int)json_integer_value(json_object_get(o, "dur_ms"));
        if (strlen(it.t.id) != 11 || it.frames <= 0 || it.dur_ms <= 0) continue;
        g_dl[g_dln++] = it;
    }
    json_decref(arr);
}

void store_init(void) {
    mx_init(&g_mx, "ytm_store");
    sceIoMkdir(DL_DIR, 0777);
    list_load(&g_fav, FAV_PATH, FAV_MAX);
    list_load(&g_hist, HIST_PATH, HIST_MAX);
    pl_load();
    dl_load();
    log_msg("store: %d favorites, %d history, %d playlists, %d downloads", g_fav.n, g_hist.n, g_pln, g_dln);
}

int store_fav_count(void) {
    mx_lock(&g_mx);
    int n = g_fav.n;
    mx_unlock(&g_mx);
    return n;
}

int store_fav_get(int i, track_t *t) {
    int ok = 0;
    mx_lock(&g_mx);
    if (i >= 0 && i < g_fav.n) {
        *t = g_fav.items[i];
        ok = 1;
    }
    mx_unlock(&g_mx);
    return ok;
}

int store_is_fav(const char *id) {
    mx_lock(&g_mx);
    int r = list_find(&g_fav, id) >= 0;
    mx_unlock(&g_mx);
    return r;
}

int store_fav_toggle(const track_t *t) {
    int now;
    mx_lock(&g_mx);
    int idx = list_find(&g_fav, t->id);
    if (idx >= 0) {
        list_remove(&g_fav, idx);
        now = 0;
    } else {
        list_push_front(&g_fav, t);
        now = 1;
    }
    list_save(&g_fav, FAV_PATH);
    mx_unlock(&g_mx);
    return now;
}

int store_hist_count(void) {
    mx_lock(&g_mx);
    int n = g_hist.n;
    mx_unlock(&g_mx);
    return n;
}

int store_hist_get(int i, track_t *t) {
    int ok = 0;
    mx_lock(&g_mx);
    if (i >= 0 && i < g_hist.n) {
        *t = g_hist.items[i];
        ok = 1;
    }
    mx_unlock(&g_mx);
    return ok;
}

void store_hist_push(const track_t *t) {
    mx_lock(&g_mx);
    int idx = list_find(&g_hist, t->id);
    if (idx >= 0) list_remove(&g_hist, idx);
    list_push_front(&g_hist, t);
    list_save(&g_hist, HIST_PATH);
    mx_unlock(&g_mx);
}

int store_pl_count(void) {
    mx_lock(&g_mx);
    int n = g_pln;
    mx_unlock(&g_mx);
    return n;
}

int store_pl_name(int pl, char *out, size_t cap) {
    int n = -1;
    mx_lock(&g_mx);
    if (pl >= 0 && pl < g_pln) {
        str_set(out, cap, g_pl[pl].name);
        n = g_pl[pl].n;
    }
    mx_unlock(&g_mx);
    return n;
}

int store_pl_track_count(int pl) {
    int n = 0;
    mx_lock(&g_mx);
    if (pl >= 0 && pl < g_pln) n = g_pl[pl].n;
    mx_unlock(&g_mx);
    return n;
}

int store_pl_track_get(int pl, int i, track_t *t) {
    int ok = 0;
    mx_lock(&g_mx);
    if (pl >= 0 && pl < g_pln && i >= 0 && i < g_pl[pl].n) {
        *t = g_pl[pl].items[i];
        ok = 1;
    }
    mx_unlock(&g_mx);
    return ok;
}

int store_pl_create(const char *name) {
    int idx = -1;
    mx_lock(&g_mx);
    if (g_pln < PL_MAX && name && name[0]) {
        playlist_t *p = &g_pl[g_pln];
        memset(p, 0, sizeof *p);
        str_set(p->name, sizeof p->name, name);
        utf8_fix(p->name);
        if (p->name[0]) {
            idx = g_pln++;
            pl_save();
        }
    }
    mx_unlock(&g_mx);
    return idx;
}

int store_pl_rename(int pl, const char *name) {
    int ok = 0;
    mx_lock(&g_mx);
    if (pl >= 0 && pl < g_pln && name && name[0]) {
        char tmp[PL_NAME_MAX];
        str_set(tmp, sizeof tmp, name);
        utf8_fix(tmp);
        if (tmp[0]) {
            memcpy(g_pl[pl].name, tmp, sizeof tmp);
            pl_save();
            ok = 1;
        }
    }
    mx_unlock(&g_mx);
    return ok;
}

int store_pl_delete(int pl) {
    int ok = 0;
    mx_lock(&g_mx);
    if (pl >= 0 && pl < g_pln) {
        pl_clear(&g_pl[pl]);
        memmove(&g_pl[pl], &g_pl[pl + 1], (size_t)(g_pln - pl - 1) * sizeof(playlist_t));
        g_pln--;
        memset(&g_pl[g_pln], 0, sizeof(playlist_t));
        pl_save();
        ok = 1;
    }
    mx_unlock(&g_mx);
    return ok;
}

int store_pl_add(int pl, const track_t *t) {
    int r = -1;
    mx_lock(&g_mx);
    if (pl >= 0 && pl < g_pln) {
        playlist_t *p = &g_pl[pl];
        int dup = 0;
        for (int i = 0; i < p->n; i++) {
            if (strcmp(p->items[i].id, t->id) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            r = 0;
        } else if (p->n < PL_TRACKS_MAX && pl_reserve(p, p->n + 1)) {
            p->items[p->n++] = *t;
            pl_save();
            r = 1;
        }
    }
    mx_unlock(&g_mx);
    return r;
}

int store_pl_remove_track(int pl, int i) {
    int ok = 0;
    mx_lock(&g_mx);
    if (pl >= 0 && pl < g_pln && i >= 0 && i < g_pl[pl].n) {
        playlist_t *p = &g_pl[pl];
        memmove(&p->items[i], &p->items[i + 1], (size_t)(p->n - i - 1) * sizeof(track_t));
        p->n--;
        pl_save();
        ok = 1;
    }
    mx_unlock(&g_mx);
    return ok;
}

int store_dl_count(void) {
    mx_lock(&g_mx);
    int n = g_dln;
    mx_unlock(&g_mx);
    return n;
}

int store_dl_get(int i, track_t *t) {
    int ok = 0;
    mx_lock(&g_mx);
    if (g_dl && i >= 0 && i < g_dln) {
        *t = g_dl[i].t;
        ok = 1;
    }
    mx_unlock(&g_mx);
    return ok;
}

static int dl_index(const char *id) {
    for (int i = 0; i < g_dln; i++)
        if (strcmp(g_dl[i].t.id, id) == 0) return i;
    return -1;
}

int store_dl_find(const char *id, int *frames, int *dur_ms) {
    int found = 0;
    mx_lock(&g_mx);
    if (g_dl) {
        int i = dl_index(id);
        if (i >= 0) {
            if (frames) *frames = g_dl[i].frames;
            if (dur_ms) *dur_ms = g_dl[i].dur_ms;
            found = 1;
        }
    }
    mx_unlock(&g_mx);
    return found;
}

void store_dl_add(const track_t *t, int frames, int dur_ms) {
    mx_lock(&g_mx);
    if (g_dl) {
        int i = dl_index(t->id);
        if (i >= 0) {
            memmove(&g_dl[i], &g_dl[i + 1], (size_t)(g_dln - i - 1) * sizeof(dl_item_t));
            g_dln--;
        }
        if (g_dln >= DL_MAX) g_dln = DL_MAX - 1;
        memmove(&g_dl[1], &g_dl[0], (size_t)g_dln * sizeof(dl_item_t));
        g_dl[0].t = *t;
        g_dl[0].frames = frames;
        g_dl[0].dur_ms = dur_ms;
        g_dln++;
        dl_save();
    }
    mx_unlock(&g_mx);
}

void store_dl_path(const char *id, const char *ext, char *out, size_t cap) {
    snprintf(out, cap, DL_DIR "/%s.%s", id, ext);
}

void store_dl_remove(const char *id) {
    char id_copy[16];
    str_set(id_copy, sizeof id_copy, id);
    int removed = 0;
    mx_lock(&g_mx);
    if (g_dl) {
        int i = dl_index(id_copy);
        if (i >= 0) {
            memmove(&g_dl[i], &g_dl[i + 1], (size_t)(g_dln - i - 1) * sizeof(dl_item_t));
            g_dln--;
            dl_save();
            removed = 1;
        }
    }
    mx_unlock(&g_mx);
    if (removed) {
        char path[96];
        store_dl_path(id_copy, "aac", path, sizeof path);
        sceIoRemove(path);
        store_dl_path(id_copy, "jpg", path, sizeof path);
        sceIoRemove(path);
    }
}
