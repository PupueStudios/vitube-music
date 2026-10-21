#include "store.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#define FAV_PATH DATA_DIR "/favorites.json"
#define HIST_PATH DATA_DIR "/history.json"
#define FAV_MAX 1000
#define HIST_MAX 100

typedef struct {
    track_t *items;
    int n;
    int cap;
} list_t;

static list_t g_fav;
static list_t g_hist;
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

void store_init(void) {
    mx_init(&g_mx, "ytm_store");
    list_load(&g_fav, FAV_PATH, FAV_MAX);
    list_load(&g_hist, HIST_PATH, HIST_MAX);
    log_msg("store: %d favorites, %d history", g_fav.n, g_hist.n);
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
