#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/gxm.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <vita2d.h>

#include "font.h"
#include "gfx.h"
#include "jobs.h"
#include "net.h"
#include "player.h"
#include "store.h"
#include "util.h"

#define HEADER_H 64
#define CHIP_Y 14
#define CHIP_H 36
#define CHIP_X0 84
#define CHIP_PAD 20
#define CHIP_GAP 10

#define LIST_Y 72
#define ROW_PITCH 60
#define ROW_H 54
#define VISIBLE 5
#define LIST_H (VISIBLE * ROW_PITCH)
#define ROW_X 16
#define ROW_W 920
#define THUMB 40

#define FOOT_X 16
#define FOOT_Y 388
#define FOOT_W 928
#define FOOT_H 104
#define FOOT_R 22
#define ART_X (FOOT_X + 16)
#define ART_Y (FOOT_Y + 12)
#define ART_SIZE 80
#define TXT_X 130
#define PB_X 180
#define PB_W 700
#define PB_Y (FOOT_Y + 78)
#define CTL_CY (FOOT_Y + 38)
#define CTL_PREV_X 668
#define CTL_PLAY_X 728
#define CTL_NEXT_X 788
#define HINT_Y 519

#define FS_L 22
#define FS_M 18
#define FS_S 16
#define FS_XS 14
#define PS_SIZE 22

#define SMOOTH(rate) (1.0f - expf(-(rate) * g_dt))

#define FLY_COUNT 16

#define COL_BG RGBA8(15, 15, 15, 255)
#define RED 255, 48, 64

enum {
    TAB_SEARCH = 0,
    TAB_QUEUE,
    TAB_FAVS,
    TAB_HIST,
    TAB_COUNT
};

typedef struct {
    float x, y, speed, phase, size, sway;
} fly_t;

typedef struct {
    const char *glyphs;
    const char *label;
} hint_t;

static const char *const TAB_NAMES[TAB_COUNT] = {"Search", "Queue", "Favorites", "History"};

static vita2d_pgf *g_font;
static GfxFont *g_ps;
static vita2d_texture *g_glow;
static vita2d_texture *g_logo;
static float g_lh = 20.0f;
static float g_alpha = 1.0f;
static float g_dt = 1.0f / 60.0f;
static float g_t;
static float g_intro;
static float g_tabfade = 1.0f;
static float g_tabdir;
static float g_prog;
static float g_hl[TAB_COUNT];
static float g_scroll[TAB_COUNT];
static fly_t g_flies[FLY_COUNT];
static int g_tab = TAB_SEARCH;
static int g_sel[TAB_COUNT];
static int g_top[TAB_COUNT];
static SceCtrlData g_pad;
static SceCtrlData g_prev;
static vita2d_texture *g_art;
static char g_art_id[16];
static char g_toast[112];
static uint32_t g_toast_from;
static uint32_t g_toast_until;
static uint32_t g_last_tick;
static uint32_t g_last_frame;
static uint32_t g_hold_since[2];
static uint32_t g_hold_last[2];
static uint32_t g_seek_last;
static int g_started;
static int g_screen_off;

static int t_down;
static int t_moved;
static int t_x0;
static int t_y0;
static int t_ly;
static int t_accum;

static int in_hangul(unsigned int c) {
    return (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F);
}

static int in_cjk(unsigned int c) {
    return (c >= 0x3000 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0x3400 && c <= 0x4DBF) ||
           (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
}

static unsigned C(int r, int g, int b, float a) {
    return gfx_color(r, g, b, a * g_alpha);
}

static void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast, sizeof g_toast, fmt, ap);
    va_end(ap);
    g_toast_from = now_ms();
    g_toast_until = g_toast_from + 2800;
}

static int tab_count(int tab) {
    switch (tab) {
    case TAB_SEARCH:
        return jobs_search_count();
    case TAB_QUEUE:
        return player_queue_count();
    case TAB_FAVS:
        return store_fav_count();
    default:
        return store_hist_count();
    }
}

static int tab_get(int tab, int i, track_t *t) {
    switch (tab) {
    case TAB_SEARCH:
        return jobs_search_get(i, t);
    case TAB_QUEUE:
        return player_queue_get(i, t);
    case TAB_FAVS:
        return store_fav_get(i, t);
    default:
        return store_hist_get(i, t);
    }
}

static void fmt_time(char *out, size_t cap, int ms) {
    if (ms < 0) ms = 0;
    int s = ms / 1000;
    if (s >= 3600) snprintf(out, cap, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

static float pgf_scale(int size) {
    return 0.93f * (float)size / g_lh;
}

static float tw(int size, const char *s) {
    return (float)vita2d_pgf_text_width(g_font, pgf_scale(size), s);
}

static void tx(float x, float cy, int size, unsigned color, const char *s) {
    vita2d_pgf_draw_text(g_font, (int)x, (int)(cy + (float)size * 0.33f), color, pgf_scale(size), s);
}

static void tx_right(float xr, float cy, int size, unsigned color, const char *s) {
    tx(xr - tw(size, s), cy, size, color, s);
}

static void tx_center(float cx, float cy, int size, unsigned color, const char *s) {
    tx(cx - tw(size, s) * 0.5f, cy, size, color, s);
}

static void tx_fit(float x, float cy, float maxw, int size, unsigned color, const char *s) {
    if (tw(size, s) <= maxw) {
        tx(x, cy, size, color, s);
        return;
    }
    char buf[300];
    str_set(buf, 280, s);
    size_t offs[300];
    size_t n = 0;
    for (size_t i = 0; buf[i] && n < 299; i++) {
        if (((unsigned char)buf[i] & 0xC0) != 0x80) offs[n++] = i;
    }
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        char tmp[310];
        size_t end = mid < n ? offs[mid] : strlen(buf);
        memcpy(tmp, buf, end);
        tmp[end] = '\0';
        strcat(tmp, "...");
        if (tw(size, tmp) <= maxw) lo = mid;
        else hi = mid - 1;
    }
    size_t end = lo < n ? offs[lo] : strlen(buf);
    char out[320];
    memcpy(out, buf, end);
    out[end] = '\0';
    strcat(out, "...");
    tx(x, cy, size, color, out);
}

static int is_current_track(const track_t *t) {
    track_t cur;
    return player_current(&cur) && strcmp(cur.id, t->id) == 0;
}

static float rnd01(unsigned int *s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)((*s >> 8) & 0xFFFFu) / 65535.0f;
}

static void flies_init(void) {
    unsigned int s = 0x5EED1234u;
    for (int i = 0; i < FLY_COUNT; i++) {
        g_flies[i].x = rnd01(&s) * SCREEN_W;
        g_flies[i].y = rnd01(&s) * SCREEN_H;
        g_flies[i].speed = 6.0f + rnd01(&s) * 14.0f;
        g_flies[i].phase = rnd01(&s) * 6.2831853f;
        g_flies[i].size = 1.0f + rnd01(&s) * 1.6f;
        g_flies[i].sway = 8.0f + rnd01(&s) * 18.0f;
    }
}

static void tri_right(float x, float cy, float w, float h, unsigned color) {
    for (float i = 0; i < w; i += 1.0f) {
        float hh = h * (1.0f - i / w);
        vita2d_draw_rectangle(x + i, cy - hh * 0.5f, 1.0f, hh, color);
    }
}

static void tri_left(float x, float cy, float w, float h, unsigned color) {
    for (float i = 0; i < w; i += 1.0f) {
        float hh = h * (1.0f - i / w);
        vita2d_draw_rectangle(x + w - 1.0f - i, cy - hh * 0.5f, 1.0f, hh, color);
    }
}

static void tri_down(float cx, float y, float w, float h, unsigned color) {
    for (float i = 0; i < h; i += 1.0f) {
        float ww = w * (1.0f - i / h);
        vita2d_draw_rectangle(cx - ww * 0.5f, y + i, ww, 1.0f, color);
    }
}

static void icon_play(float cx, float cy, float s, unsigned color) {
    float w = s * 0.86f;
    tri_right(cx - w * 0.38f, cy, w, s, color);
}

static void icon_pause(float cx, float cy, float s, unsigned color) {
    float bw = s * 0.28f;
    gfx_rounded_rect(cx - s * 0.36f, cy - s * 0.5f, bw, s, 1.5f, color);
    gfx_rounded_rect(cx + s * 0.36f - bw, cy - s * 0.5f, bw, s, 1.5f, color);
}

static void icon_next(float cx, float cy, float s, unsigned color) {
    tri_right(cx - s * 0.5f, cy, s * 0.78f, s, color);
    gfx_rounded_rect(cx + s * 0.3f, cy - s * 0.5f, s * 0.2f, s, 1.0f, color);
}

static void icon_prev(float cx, float cy, float s, unsigned color) {
    tri_left(cx - s * 0.28f, cy, s * 0.78f, s, color);
    gfx_rounded_rect(cx - s * 0.5f, cy - s * 0.5f, s * 0.2f, s, 1.0f, color);
}

static void icon_heart(float cx, float cy, float s, unsigned color) {
    float r = s * 0.27f;
    vita2d_draw_fill_circle(cx - r * 0.95f, cy - s * 0.14f, r, color);
    vita2d_draw_fill_circle(cx + r * 0.95f, cy - s * 0.14f, r, color);
    tri_down(cx, cy - s * 0.12f, r * 4.0f - 1.0f, s * 0.68f, color);
}

static void icon_search(float cx, float cy, float s, unsigned color) {
    float r = s * 0.36f;
    gfx_rounded_ring(cx - r - s * 0.1f, cy - r - s * 0.1f, r * 2.0f, r * 2.0f, r, 2.2f, color);
    float x0 = cx + r * 0.62f;
    float y0 = cy + r * 0.62f;
    for (int i = -1; i <= 1; i++) {
        vita2d_draw_line(x0 + (float)i * 0.7f, y0 - (float)i * 0.7f, x0 + s * 0.42f + (float)i * 0.7f,
                         y0 + s * 0.42f - (float)i * 0.7f, color);
    }
}

static void icon_note(float cx, float cy, float s, unsigned color) {
    vita2d_draw_fill_circle(cx - s * 0.18f, cy + s * 0.28f, s * 0.2f, color);
    vita2d_draw_rectangle(cx - s * 0.02f, cy - s * 0.4f, s * 0.1f, s * 0.7f, color);
    vita2d_draw_rectangle(cx - s * 0.02f, cy - s * 0.4f, s * 0.34f, s * 0.12f, color);
}

static void draw_eq(float cx, float cy, float s, int live, unsigned color) {
    for (int i = 0; i < 3; i++) {
        float k = live ? 0.5f + 0.5f * sinf(g_t * (6.0f + (float)i * 2.1f) + (float)i * 1.7f) : 0.4f;
        float h = s * (0.28f + 0.72f * k);
        float bw = s * 0.22f;
        float x = cx - s * 0.5f + (float)i * (bw + s * 0.1f);
        gfx_rounded_rect(x, cy + s * 0.5f - h, bw, h, 1.5f, color);
    }
}

static void glass(float x, float y, float w, float h, float r, float top_a, float bot_a, float ring_a) {
    gfx_rounded_rect_vgrad(x, y, w, h, r, C(44, 44, 48, top_a), C(24, 24, 27, bot_a));
    gfx_rounded_rect_vgrad(x + 2.0f, y + 2.0f, w - 4.0f, h * 0.4f, r - 2.0f, C(255, 255, 255, 0.07), C(255, 255, 255, 0.0));
    gfx_rounded_ring(x, y, w, h, r, 1.0f, C(255, 255, 255, ring_a));
}

static void chip_rect(int i, float *x, float *w) {
    float cx = CHIP_X0;
    for (int k = 0; k < i; k++) cx += tw(FS_M, TAB_NAMES[k]) + CHIP_PAD * 2.0f + CHIP_GAP;
    *x = cx;
    *w = tw(FS_M, TAB_NAMES[i]) + CHIP_PAD * 2.0f;
}

static void draw_background(void) {
    float e = gfx_ease_out_cubic(gfx_clamp01(g_intro));
    gfx_draw_glow(g_glow, 140.0f + sinf(g_t * 0.25f) * 30.0f, -40.0f, 560.0f, 255, 24, 48, 0.17f * e);
    gfx_draw_glow(g_glow, SCREEN_W - 60.0f, SCREEN_H * 0.5f + cosf(g_t * 0.2f) * 20.0f, 460.0f, 255, 255, 255, 0.035f * e);
    gfx_draw_glow(g_glow, SCREEN_W * 0.5f, SCREEN_H + 60.0f, 520.0f, 255, 24, 48, 0.10f * e);

    for (int i = 0; i < FLY_COUNT; i++) {
        const fly_t *f = &g_flies[i];
        float fx = f->x + sinf(g_t * 0.7f + f->phase) * f->sway;
        float tws = 0.5f + 0.5f * sinf(g_t * 1.6f + f->phase);
        float a = (0.10f + 0.20f * tws) * e;
        int red = (i % 3) == 0;
        vita2d_draw_fill_circle(fx, f->y, f->size * 3.2f, gfx_color(255, red ? 70 : 255, red ? 90 : 255, a * 0.20f));
        vita2d_draw_fill_circle(fx, f->y, f->size, gfx_color(255, red ? 150 : 255, red ? 160 : 255, a));
    }
}

static void draw_header(void) {
    float e = gfx_ease_out_cubic(gfx_clamp01(g_intro));
    g_alpha = e;

    float ls = 44.0f / (float)vita2d_texture_get_width(g_logo);
    vita2d_draw_texture_tint_scale(g_logo, 24.0f, 10.0f, ls, ls, gfx_color(255, 255, 255, g_alpha));

    for (int i = 0; i < TAB_COUNT; i++) {
        float x, w;
        chip_rect(i, &x, &w);
        int on = (i == g_tab);
        if (on) {
            gfx_rounded_rect(x, CHIP_Y, w, CHIP_H, CHIP_H * 0.5f, C(255, 255, 255, 0.96));
            tx(x + CHIP_PAD, CHIP_Y + CHIP_H * 0.5f, FS_M, C(18, 18, 18, 1.0), TAB_NAMES[i]);
        } else {
            gfx_rounded_rect(x, CHIP_Y, w, CHIP_H, CHIP_H * 0.5f, C(255, 255, 255, 0.09));
            gfx_rounded_ring(x, CHIP_Y, w, CHIP_H, CHIP_H * 0.5f, 1.0f, C(255, 255, 255, 0.10));
            tx(x + CHIP_PAD, CHIP_Y + CHIP_H * 0.5f, FS_M, C(255, 255, 255, 0.72), TAB_NAMES[i]);
        }
    }

    if (jobs_search_busy()) {
        for (int i = 0; i < 3; i++) {
            float k = 0.5f + 0.5f * sinf(g_t * 7.0f - (float)i * 0.9f);
            vita2d_draw_fill_circle(690.0f + (float)i * 14.0f, 32.0f, 3.0f + 1.5f * k, C(RED, 0.4f + 0.6f * k));
        }
    }

    int pct = scePowerGetBatteryLifePercent();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int chg = scePowerIsBatteryCharging();
    char b[16];
    snprintf(b, sizeof b, "%d%%", pct);
    float bx = SCREEN_W - 20.0f;
    tx_right(bx, 32.0f, FS_XS, C(255, 255, 255, 0.62), b);
    float ix = bx - tw(FS_XS, b) - 38.0f;
    gfx_rounded_ring(ix, 25.5f, 26.0f, 13.0f, 4.0f, 1.5f, C(255, 255, 255, 0.55));
    vita2d_draw_rectangle(ix + 26.0f, 29.0f, 2.0f, 6.0f, C(255, 255, 255, 0.55));
    float fw = 20.0f * (float)pct / 100.0f;
    if (fw > 1.0f) {
        unsigned bc = chg ? C(90, 220, 130, 0.95) : (pct <= 15 ? C(RED, 0.95) : C(255, 255, 255, 0.80));
        gfx_rounded_rect(ix + 3.0f, 28.5f, fw, 7.0f, 2.0f, bc);
    }

    vita2d_draw_rectangle(0.0f, HEADER_H - 1.0f, SCREEN_W, 1.0f, C(255, 255, 255, 0.07));
    g_alpha = 1.0f;
}

static void draw_empty(void) {
    char msg[160] = "";
    char sub[96] = "";
    int err = 0;
    int searching = 0;
    if (g_tab == TAB_SEARCH) {
        if (jobs_search_busy()) {
            snprintf(msg, sizeof msg, "Searching");
            searching = 1;
        } else {
            jobs_search_error(msg, sizeof msg);
            if (msg[0]) {
                err = 1;
            } else {
                snprintf(msg, sizeof msg, "Search YouTube Music");
                snprintf(sub, sizeof sub, "Press SELECT or tap here");
            }
        }
    } else if (g_tab == TAB_QUEUE) {
        snprintf(msg, sizeof msg, "The queue is empty");
        snprintf(sub, sizeof sub, "Press O on a song to add it");
    } else if (g_tab == TAB_FAVS) {
        snprintf(msg, sizeof msg, "No favorites yet");
        snprintf(sub, sizeof sub, "Press TRIANGLE on a song to save it");
    } else {
        snprintf(msg, sizeof msg, "Nothing played yet");
        snprintf(sub, sizeof sub, "Songs you play will show up here");
    }

    float cx = SCREEN_W * 0.5f;
    float cy = LIST_Y + LIST_H * 0.5f - 26.0f;
    float pulse = 0.5f + 0.5f * sinf(g_t * 2.2f);

    gfx_draw_glow(g_glow, cx, cy, 120.0f + 10.0f * pulse, RED, 0.16f);
    gfx_rounded_rect_vgrad(cx - 42.0f, cy - 42.0f, 84.0f, 84.0f, 42.0f, C(255, 255, 255, 0.12), C(255, 255, 255, 0.05));
    gfx_rounded_ring(cx - 42.0f, cy - 42.0f, 84.0f, 84.0f, 42.0f, 1.0f, C(255, 255, 255, 0.16));

    if (g_tab == TAB_SEARCH) icon_search(cx, cy, 36.0f, C(255, 255, 255, 0.85));
    else if (g_tab == TAB_QUEUE) icon_note(cx, cy, 36.0f, C(255, 255, 255, 0.85));
    else if (g_tab == TAB_FAVS) icon_heart(cx, cy + 1.0f, 36.0f, C(255, 255, 255, 0.85));
    else icon_play(cx, cy, 28.0f, C(255, 255, 255, 0.85));

    if (searching) {
        float w = tw(FS_L, msg);
        tx(cx - w * 0.5f - 12.0f, cy + 78.0f, FS_L, C(255, 255, 255, 0.92), msg);
        for (int i = 0; i < 3; i++) {
            float k = 0.5f + 0.5f * sinf(g_t * 7.0f - (float)i * 0.9f);
            vita2d_draw_fill_circle(cx + w * 0.5f - 4.0f + (float)i * 10.0f, cy + 85.0f, 2.2f + 1.2f * k, C(RED, 0.4f + 0.6f * k));
        }
    } else if (err) {
        tx_fit(cx - 330.0f, cy + 78.0f, 660.0f, FS_M, C(255, 110, 120, 1.0), msg);
        tx_center(cx, cy + 104.0f, FS_XS, C(255, 255, 255, 0.50), "Press SELECT to try again");
    } else {
        tx_center(cx, cy + 78.0f, FS_L, C(255, 255, 255, 0.92), msg);
        tx_center(cx, cy + 106.0f, FS_S, C(255, 255, 255, 0.50), sub);
    }
}

static void draw_row(int idx, const track_t *t, float y, int sel) {
    int playing = is_current_track(t);
    pl_state_t st = player_state();
    float x = ROW_X + 24.0f;
    float ty = y + (ROW_H - THUMB) * 0.5f;

    gfx_rounded_rect(x, ty, THUMB, THUMB, 8.0f, C(255, 255, 255, 0.07));
    if (playing) {
        if (g_art && strcmp(g_art_id, t->id) == 0) {
            float sx = THUMB / (float)vita2d_texture_get_width(g_art);
            float sy = THUMB / (float)vita2d_texture_get_height(g_art);
            vita2d_draw_texture_tint_scale(g_art, x, ty, sx, sy, gfx_color(255, 255, 255, g_alpha));
        }
        vita2d_draw_rectangle(x, ty, THUMB, THUMB, C(0, 0, 0, 0.45));
        draw_eq(x + THUMB * 0.5f, ty + THUMB * 0.5f, 18.0f, st == PL_PLAYING, C(255, 255, 255, 0.96));
    } else {
        char n[12];
        snprintf(n, sizeof n, "%d", idx + 1);
        tx_center(x + THUMB * 0.5f, ty + THUMB * 0.5f, FS_S, C(255, 255, 255, sel ? 0.75 : 0.42), n);
    }

    char dur[16] = "";
    if (t->duration > 0) fmt_time(dur, sizeof dur, t->duration * 1000);
    float right = ROW_X + ROW_W - 22.0f;
    float dw = dur[0] ? tw(FS_S, dur) : 0.0f;
    if (dur[0]) tx_right(right, y + ROW_H * 0.5f, FS_S, C(255, 255, 255, sel ? 0.78 : 0.55), dur);
    float lim = right - dw;
    if (store_is_fav(t->id)) {
        icon_heart(right - dw - 22.0f, y + ROW_H * 0.5f, 16.0f, C(255, 82, 102, 1.0));
        lim = right - dw - 42.0f;
    }

    float tx0 = x + THUMB + 16.0f;
    float maxw = lim - tx0 - 14.0f;
    unsigned tc = playing ? C(255, 88, 100, 1.0) : C(255, 255, 255, sel ? 1.0 : 0.92);
    tx_fit(tx0, y + 19.0f, maxw, FS_M, tc, t->title);
    char sub[220];
    if (t->artist[0] && t->album[0]) snprintf(sub, sizeof sub, "%s  -  %s", t->artist, t->album);
    else snprintf(sub, sizeof sub, "%s", t->artist[0] ? t->artist : t->album);
    tx_fit(tx0, y + 38.0f, maxw, FS_XS, C(255, 255, 255, sel ? 0.66 : 0.50), sub);
}

static void draw_list(void) {
    float e = gfx_ease_out_cubic(gfx_clamp01(g_intro));
    float base = e * gfx_ease_out_cubic(gfx_clamp01(g_tabfade));
    g_alpha = base;

    int count = tab_count(g_tab);
    if (count == 0) {
        draw_empty();
        g_alpha = 1.0f;
        return;
    }

    float slide = (1.0f - gfx_ease_out_cubic(gfx_clamp01(g_tabfade))) * 36.0f * g_tabdir;
    float scroll = g_scroll[g_tab];

    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(0, LIST_Y, (int)SCREEN_W - 1, LIST_Y + LIST_H - 1);

    float hy = LIST_Y + (g_hl[g_tab] - scroll) * ROW_PITCH + (ROW_PITCH - ROW_H) * 0.5f;
    float pulse = 0.5f + 0.5f * sinf(g_t * 3.0f);
    gfx_rounded_rect_vgrad(ROW_X + slide, hy, ROW_W, ROW_H, 16.0f, C(255, 255, 255, 0.15), C(255, 255, 255, 0.05));
    gfx_rounded_ring(ROW_X + slide, hy, ROW_W, ROW_H, 16.0f, 1.0f, C(255, 255, 255, 0.20));
    gfx_rounded_rect(ROW_X + slide + 9.0f, hy + (ROW_H - 26.0f) * 0.5f, 4.0f, 26.0f, 2.0f, C(RED, 0.80f + 0.20f * pulse));

    int first = (int)floorf(scroll);
    if (first < 0) first = 0;
    for (int idx = first; idx < first + VISIBLE + 2 && idx < count; idx++) {
        track_t t;
        if (!tab_get(g_tab, idx, &t)) break;
        float y = LIST_Y + ((float)idx - scroll) * ROW_PITCH + (ROW_PITCH - ROW_H) * 0.5f;
        draw_row(idx, &t, y, idx == g_sel[g_tab]);
    }
    vita2d_disable_clipping();

    if (count > VISIBLE) {
        float th = LIST_H - 8.0f;
        float bh = th * VISIBLE / (float)count;
        if (bh < 28.0f) bh = 28.0f;
        float range = (float)(count - VISIBLE);
        float f = range > 0.0f ? gfx_clamp01(scroll / range) : 0.0f;
        float by = LIST_Y + 4.0f + (th - bh) * f;
        gfx_rounded_rect(SCREEN_W - 12.0f, LIST_Y + 4.0f, 4.0f, th, 2.0f, C(255, 255, 255, 0.05));
        gfx_rounded_rect(SCREEN_W - 12.0f, by, 4.0f, bh, 2.0f, C(255, 255, 255, 0.32));
    }
    g_alpha = 1.0f;
}

static void draw_circle_btn(float cx, float cy, float r, int filled) {
    if (filled) {
        gfx_draw_glow(g_glow, cx, cy, r * 2.6f, 255, 255, 255, 0.14f * g_alpha);
        gfx_rounded_rect_vgrad(cx - r, cy - r, r * 2.0f, r * 2.0f, r, C(255, 255, 255, 1.0), C(226, 226, 230, 1.0));
    } else {
        gfx_rounded_rect(cx - r, cy - r, r * 2.0f, r * 2.0f, r, C(255, 255, 255, 0.08));
    }
}

static void draw_footer(void) {
    float e = gfx_ease_out_cubic(gfx_clamp01(g_intro));
    float oy = (1.0f - e) * 36.0f;
    g_alpha = e;

    float py = FOOT_Y + oy;
    glass(FOOT_X, py, FOOT_W, FOOT_H, FOOT_R, 0.94f, 0.96f, 0.12f);

    track_t cur;
    int has = player_current(&cur);
    pl_state_t st = player_state();

    float ax = ART_X;
    float ay = ART_Y + oy;
    gfx_draw_glow(g_glow, ax + ART_SIZE * 0.5f, ay + ART_SIZE * 0.5f, 96.0f, RED, has ? 0.20f * g_alpha : 0.0f);
    gfx_rounded_rect(ax, ay, ART_SIZE, ART_SIZE, 10.0f, C(255, 255, 255, 0.07));
    if (has && g_art && strcmp(g_art_id, cur.id) == 0) {
        float sx = ART_SIZE / (float)vita2d_texture_get_width(g_art);
        float sy = ART_SIZE / (float)vita2d_texture_get_height(g_art);
        vita2d_draw_texture_tint_scale(g_art, ax, ay, sx, sy, gfx_color(255, 255, 255, g_alpha));
    } else {
        icon_note(ax + ART_SIZE * 0.5f, ay + ART_SIZE * 0.5f, 30.0f, C(255, 255, 255, 0.22));
    }
    gfx_rounded_ring(ax - 1.0f, ay - 1.0f, ART_SIZE + 2.0f, ART_SIZE + 2.0f, 4.0f, 1.0f, C(255, 255, 255, 0.18));

    char errmsg[200] = "";
    if (st == PL_ERROR) player_message(errmsg, sizeof errmsg);

    float tmax = CTL_PREV_X - 40.0f - TXT_X;
    if (!has) {
        tx(TXT_X, py + 28.0f, FS_L, C(255, 255, 255, 0.55), "Nothing playing");
        tx(TXT_X, py + 52.0f, FS_S, C(255, 255, 255, 0.35), "Pick a song to start");
    } else {
        tx_fit(TXT_X, py + 28.0f, tmax, FS_L, C(255, 255, 255, 0.98), cur.title);
        if (errmsg[0]) {
            char l1[110];
            str_set(l1, 96, errmsg);
            tx_fit(TXT_X, py + 50.0f, tmax + 60.0f, FS_XS, C(255, 110, 120, 1.0), l1);
        } else {
            tx_fit(TXT_X, py + 51.0f, tmax, FS_S, C(255, 255, 255, 0.60), cur.artist);
        }
    }

    float cy = CTL_CY + oy;
    int playing = st == PL_PLAYING;
    draw_circle_btn(CTL_PREV_X, cy, 20.0f, 0);
    icon_prev(CTL_PREV_X, cy, 16.0f, C(255, 255, 255, 0.92));
    draw_circle_btn(CTL_PLAY_X, cy, 25.0f, 1);
    if (st == PL_LOADING) {
        for (int i = 0; i < 3; i++) {
            float k = 0.5f + 0.5f * sinf(g_t * 8.0f - (float)i * 1.0f);
            vita2d_draw_fill_circle(CTL_PLAY_X - 10.0f + (float)i * 10.0f, cy, 2.4f + 1.4f * k, C(20, 20, 20, 0.5f + 0.5f * k));
        }
    } else if (playing) {
        icon_pause(CTL_PLAY_X, cy, 20.0f, C(18, 18, 18, 1.0));
    } else {
        icon_play(CTL_PLAY_X + 1.0f, cy, 20.0f, C(18, 18, 18, 1.0));
    }
    draw_circle_btn(CTL_NEXT_X, cy, 20.0f, 0);
    icon_next(CTL_NEXT_X, cy, 16.0f, C(255, 255, 255, 0.92));

    float rx = FOOT_X + FOOT_W - 24.0f;
    char status[64] = "";
    unsigned scol = C(255, 255, 255, 0.55);
    if (st == PL_LOADING) {
        snprintf(status, sizeof status, "Loading");
        scol = C(255, 255, 255, 0.45f + 0.45f * (0.5f + 0.5f * sinf(g_t * 5.0f)));
    } else if (st == PL_PAUSED) {
        snprintf(status, sizeof status, "Paused");
    } else if (st == PL_ERROR) {
        snprintf(status, sizeof status, "Error - see log.txt");
        scol = C(255, 110, 120, 1.0);
    } else if (st == PL_PLAYING) {
        snprintf(status, sizeof status, "Playing");
        scol = C(RED, 1.0);
    }
    if (status[0]) tx_right(rx, py + 28.0f, FS_XS, scol, status);

    int pos = player_position_ms();
    int dur = player_duration_ms();
    if (st == PL_LOADING || st == PL_ERROR || st == PL_IDLE) pos = 0;
    float frac = 0.0f;
    if (dur > 0 && has) frac = gfx_clamp01(pos / (float)dur);
    if (fabsf(frac - g_prog) > 0.2f) g_prog = frac;
    else g_prog += (frac - g_prog) * SMOOTH(18.0f);

    float by = PB_Y + oy;
    gfx_rounded_rect(PB_X, by - 2.5f, PB_W, 5.0f, 2.5f, C(255, 255, 255, 0.16));
    if (g_prog > 0.0f) {
        float fw = PB_W * g_prog;
        if (fw < 5.0f) fw = 5.0f;
        gfx_rounded_rect_vgrad(PB_X, by - 2.5f, fw, 5.0f, 2.5f, C(255, 84, 96, 1.0), C(RED, 1.0));
        gfx_draw_glow(g_glow, PB_X + fw, by, 20.0f, RED, 0.55f * g_alpha);
        vita2d_draw_fill_circle(PB_X + fw, by, 6.5f, C(255, 255, 255, 1.0));
    }

    char a[24] = "0:00";
    char b[24] = "0:00";
    if (has) {
        fmt_time(a, sizeof a, pos);
        fmt_time(b, sizeof b, dur);
    }
    tx(TXT_X, by, FS_XS, C(255, 255, 255, 0.60), a);
    tx(PB_X + PB_W + 14.0f, by, FS_XS, C(255, 255, 255, 0.60), b);
    g_alpha = 1.0f;
}

static void draw_hints(void) {
    static const hint_t h[] = {
        {"34", "Move"}, {"21", "Tabs"}, {"x", "Play"},   {"c", "Queue"},
        {"s", "Pause"}, {"t", "Favorite"}, {"lr", "Skip"}, {"b", "Search"}, {"a", "Screen off"},
    };
    const int n = (int)(sizeof h / sizeof h[0]);
    float e = gfx_ease_out_cubic(gfx_clamp01(g_intro));
    g_alpha = e;

    const float igap = 6.0f;
    const float gap = 20.0f;
    float iw[9];
    float total = 0.0f;
    for (int i = 0; i < n; i++) {
        iw[i] = (float)gfx_font_width(g_ps, PS_SIZE, h[i].glyphs);
        total += iw[i] + igap + tw(FS_XS, h[i].label);
        if (i < n - 1) total += gap;
    }

    float pw = total + 44.0f;
    float px = SCREEN_W * 0.5f - pw * 0.5f;
    gfx_rounded_rect(px, HINT_Y - 17.0f, pw, 34.0f, 17.0f, C(255, 255, 255, 0.06));
    gfx_rounded_ring(px, HINT_Y - 17.0f, pw, 34.0f, 17.0f, 1.0f, C(255, 255, 255, 0.10));

    float x = SCREEN_W * 0.5f - total * 0.5f;
    for (int i = 0; i < n; i++) {
        gfx_font_draw_icon(g_ps, x, HINT_Y, PS_SIZE, C(255, 255, 255, 0.90), h[i].glyphs);
        tx(x + iw[i] + igap, HINT_Y, FS_XS, C(255, 255, 255, 0.62), h[i].label);
        x += iw[i] + igap + tw(FS_XS, h[i].label) + gap;
    }
    g_alpha = 1.0f;
}

static void draw_toast(void) {
    uint32_t now = now_ms();
    if (now >= g_toast_until || !g_toast[0]) return;
    float in = gfx_clamp01((float)(now - g_toast_from) / 220.0f);
    float out = gfx_clamp01((float)(g_toast_until - now) / 260.0f);
    float a = gfx_ease_out_cubic(in) * out;
    g_alpha = a;

    float w = tw(FS_M, g_toast) + 56.0f;
    if (w > 820.0f) w = 820.0f;
    float h = 44.0f;
    float x = (SCREEN_W - w) * 0.5f;
    float y = FOOT_Y - 62.0f + (1.0f - gfx_ease_out_cubic(in)) * 14.0f;

    gfx_draw_glow(g_glow, SCREEN_W * 0.5f, y + h * 0.5f, w * 0.6f, 0, 0, 0, 0.45f * a);
    gfx_rounded_rect(x, y, w, h, h * 0.5f, C(36, 36, 40, 0.97));
    gfx_rounded_ring(x, y, w, h, h * 0.5f, 1.0f, C(255, 255, 255, 0.16));
    vita2d_draw_fill_circle(x + 20.0f, y + h * 0.5f, 4.0f, C(RED, 1.0));
    tx_fit(x + 34.0f, y + h * 0.5f, w - 54.0f, FS_M, C(255, 255, 255, 0.96), g_toast);
    g_alpha = 1.0f;
}

static void draw_all(void) {
    g_alpha = 1.0f;
    draw_background();
    draw_header();
    draw_list();
    draw_footer();
    draw_hints();
    draw_toast();
}

static void render_black(void) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_rectangle(0.0f, 0.0f, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 255));
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void set_screen_off(int off) {
    t_down = 0;
    if (!off) {
        g_last_frame = 0;
        g_intro = 0.35f;
    }
    g_screen_off = off;
}

static void render_frame(int with_dialog) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    draw_all();
    vita2d_end_drawing();
    if (with_dialog) vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

static int ime_run(const char *title, const char *initial, char *out, size_t cap) {
    static uint16_t u_title[64];
    static uint16_t u_init[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
    static uint16_t u_in[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
    memset(u_title, 0, sizeof u_title);
    memset(u_init, 0, sizeof u_init);
    memset(u_in, 0, sizeof u_in);
    utf8_to_utf16(title, u_title, 64);
    utf8_to_utf16(initial, u_init, 128);

    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0x0001FFFF;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.maxTextLength = 128;
    p.title = u_title;
    p.initialText = u_init;
    p.inputTextBuffer = u_in;
    if (sceImeDialogInit(&p) < 0) return 0;
    while (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) render_frame(1);
    SceImeDialogResult res;
    memset(&res, 0, sizeof res);
    sceImeDialogGetResult(&res);
    int ok = 0;
    if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        utf16_to_utf8(u_in, out, cap);
        ok = out[0] != '\0';
    }
    sceImeDialogTerm();
    return ok;
}

static void start_search(void) {
    char q[200];
    char last[160];
    jobs_search_query(last, sizeof last);
    if (!ime_run("Search YouTube Music", last, q, sizeof q)) return;
    if (!net_wifi_connected()) toast("Wi-Fi is not connected");
    g_tab = TAB_SEARCH;
    g_sel[TAB_SEARCH] = 0;
    g_top[TAB_SEARCH] = 0;
    g_hl[TAB_SEARCH] = 0.0f;
    g_scroll[TAB_SEARCH] = 0.0f;
    jobs_search(q);
}

static void clamp_tab(int tab) {
    int count = tab_count(tab);
    if (g_sel[tab] >= count) g_sel[tab] = count > 0 ? count - 1 : 0;
    if (g_top[tab] > count - VISIBLE) g_top[tab] = count - VISIBLE;
    if (g_top[tab] < 0) g_top[tab] = 0;
}

static void ensure_visible(int tab) {
    if (g_sel[tab] < g_top[tab]) g_top[tab] = g_sel[tab];
    if (g_sel[tab] >= g_top[tab] + VISIBLE) g_top[tab] = g_sel[tab] - VISIBLE + 1;
    if (g_top[tab] < 0) g_top[tab] = 0;
}

static void move_sel(int delta) {
    int n = tab_count(g_tab);
    if (n == 0) return;
    int s = g_sel[g_tab] + delta;
    if (s < 0) s = 0;
    if (s >= n) s = n - 1;
    g_sel[g_tab] = s;
    ensure_visible(g_tab);
}

static void set_tab(int tab, float dir) {
    g_tab = tab;
    g_tabdir = dir;
    g_tabfade = 0.0f;
    clamp_tab(tab);
    g_hl[tab] = (float)g_sel[tab];
    g_scroll[tab] = (float)g_top[tab];
}

static void switch_tab(int delta) {
    set_tab((g_tab + delta + TAB_COUNT) % TAB_COUNT, delta >= 0 ? 1.0f : -1.0f);
}

static void select_tab(int tab) {
    if (tab != g_tab) set_tab(tab, tab > g_tab ? 1.0f : -1.0f);
}

static void play_at(int tab, int idx) {
    int n = tab_count(tab);
    if (idx < 0 || idx >= n) return;
    track_t t;
    if (!tab_get(tab, idx, &t)) return;
    if (tab == TAB_QUEUE) {
        player_play_index(idx);
        return;
    }
    if (tab == TAB_SEARCH) {
        player_queue_set(&t, 1, 0);
        return;
    }
    track_t *arr = malloc(sizeof(track_t) * (size_t)n);
    if (!arr) return;
    int m = 0;
    for (int i = 0; i < n; i++)
        if (tab_get(tab, i, &arr[m])) m++;
    int start = idx < m ? idx : 0;
    player_queue_set(arr, m, start);
    free(arr);
}

static void add_selected_to_queue(void) {
    if (g_tab == TAB_QUEUE) return;
    track_t t;
    if (!tab_get(g_tab, g_sel[g_tab], &t)) return;
    if (player_queue_add(&t)) toast("Added to queue: %s", t.title);
    else toast("Already in the queue");
}

static void toggle_fav_selected(void) {
    track_t t;
    if (!tab_get(g_tab, g_sel[g_tab], &t)) return;
    int now = store_fav_toggle(&t);
    toast(now ? "Added to favorites" : "Removed from favorites");
}

static int pressed(unsigned btn) {
    return (g_pad.buttons & btn) && !(g_prev.buttons & btn);
}

static int repeat_fire(int slot, int active) {
    uint32_t t = now_ms();
    if (!active) {
        g_hold_since[slot] = 0;
        return 0;
    }
    if (g_hold_since[slot] == 0) {
        g_hold_since[slot] = t;
        g_hold_last[slot] = t;
        return 1;
    }
    if (t - g_hold_since[slot] > 380 && t - g_hold_last[slot] > 70) {
        g_hold_last[slot] = t;
        return 1;
    }
    return 0;
}

static int near_pt(int x, int y, float cx, float cy, float r) {
    float dx = (float)x - cx;
    float dy = (float)y - cy;
    return dx * dx + dy * dy <= r * r;
}

static void handle_tap(int x, int y) {
    if (y < HEADER_H) {
        for (int i = 0; i < TAB_COUNT; i++) {
            float cx, cw;
            chip_rect(i, &cx, &cw);
            if ((float)x >= cx - 4.0f && (float)x <= cx + cw + 4.0f && y >= CHIP_Y - 6 && y <= CHIP_Y + CHIP_H + 6) {
                if (i == g_tab && i == TAB_SEARCH) start_search();
                select_tab(i);
                return;
            }
        }
        return;
    }
    if (y >= LIST_Y && y < LIST_Y + LIST_H) {
        int row = g_top[g_tab] + (y - LIST_Y) / ROW_PITCH;
        if (row < tab_count(g_tab)) {
            g_sel[g_tab] = row;
            play_at(g_tab, row);
        } else if (g_tab == TAB_SEARCH && tab_count(TAB_SEARCH) == 0 && !jobs_search_busy()) {
            start_search();
        }
        return;
    }
    if (y >= PB_Y - 18 && y <= PB_Y + 18 && x >= PB_X - 8 && x <= PB_X + PB_W + 8) {
        int dur = player_duration_ms();
        pl_state_t st = player_state();
        if (dur > 0 && (st == PL_PLAYING || st == PL_PAUSED)) {
            float frac = (x - PB_X) / (float)PB_W;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            player_seek_abs((int)(frac * dur));
        }
        return;
    }
    if (near_pt(x, y, CTL_PREV_X, CTL_CY, 28.0f)) {
        player_prev();
        return;
    }
    if (near_pt(x, y, CTL_NEXT_X, CTL_CY, 28.0f)) {
        player_next();
        return;
    }
    if (near_pt(x, y, CTL_PLAY_X, CTL_CY, 32.0f)) player_toggle_pause();
}

static void handle_touch(void) {
    SceTouchData td;
    memset(&td, 0, sizeof td);
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
    if (td.reportNum > 0) {
        int x = td.report[0].x / 2;
        int y = td.report[0].y / 2;
        if (!t_down) {
            t_down = 1;
            t_moved = 0;
            t_x0 = x;
            t_y0 = y;
            t_ly = y;
            t_accum = 0;
        } else {
            int dx = x - t_x0;
            int dy = y - t_y0;
            if (!t_moved && (dx * dx + dy * dy) > 14 * 14) t_moved = 1;
            if (t_moved && t_y0 >= LIST_Y && t_y0 < LIST_Y + LIST_H) {
                t_accum += y - t_ly;
                int n = tab_count(g_tab);
                while (t_accum >= ROW_PITCH) {
                    g_top[g_tab]--;
                    t_accum -= ROW_PITCH;
                }
                while (t_accum <= -ROW_PITCH) {
                    g_top[g_tab]++;
                    t_accum += ROW_PITCH;
                }
                if (g_top[g_tab] > n - VISIBLE) g_top[g_tab] = n - VISIBLE;
                if (g_top[g_tab] < 0) g_top[g_tab] = 0;
            }
            t_ly = y;
        }
    } else if (t_down) {
        t_down = 0;
        if (!t_moved) handle_tap(t_x0, t_y0);
    }
}

static void update_art(void) {
    char id[16];
    uint8_t *data = NULL;
    size_t len = 0;
    if (!jobs_art_take(id, &data, &len)) return;
    vita2d_texture *tex = vita2d_load_JPEG_buffer(data, (unsigned long)len);
    free(data);
    if (!tex) {
        log_msg("art: jpeg decode failed for %s", id);
        return;
    }
    vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    if (g_art) {
        vita2d_wait_rendering_done();
        vita2d_free_texture(g_art);
    }
    g_art = tex;
    str_set(g_art_id, sizeof g_art_id, id);
}

static void handle_buttons(void) {
    if (pressed(SCE_CTRL_SELECT)) start_search();
    if (pressed(SCE_CTRL_CROSS)) play_at(g_tab, g_sel[g_tab]);
    if (pressed(SCE_CTRL_CIRCLE)) add_selected_to_queue();
    if (pressed(SCE_CTRL_SQUARE)) player_toggle_pause();
    if (pressed(SCE_CTRL_TRIANGLE)) toggle_fav_selected();
    if (pressed(SCE_CTRL_LTRIGGER)) player_prev();
    if (pressed(SCE_CTRL_RTRIGGER)) player_next();
    if (pressed(SCE_CTRL_LEFT)) switch_tab(-1);
    if (pressed(SCE_CTRL_RIGHT)) switch_tab(1);

    int up = (g_pad.buttons & SCE_CTRL_UP) || g_pad.ly < 60;
    int down = (g_pad.buttons & SCE_CTRL_DOWN) || g_pad.ly > 196;
    if (repeat_fire(0, up)) move_sel(-1);
    if (repeat_fire(1, down)) move_sel(1);

    uint32_t t = now_ms();
    int rx = (int)g_pad.rx - 128;
    if (rx > 70 || rx < -70) {
        if (t - g_seek_last > 280) {
            g_seek_last = t;
            player_seek_rel(rx > 0 ? 8000 : -8000);
        }
    }
}

static void animate(void) {
    uint32_t t = now_ms();
    if (g_last_frame == 0) g_dt = 1.0f / 60.0f;
    else g_dt = (float)(t - g_last_frame) / 1000.0f;
    g_last_frame = t;
    if (g_dt < 0.004f) g_dt = 0.004f;
    if (g_dt > 0.05f) g_dt = 0.05f;

    g_t += g_dt;
    if (g_intro < 1.0f) g_intro = fminf(1.0f, g_intro + g_dt / 0.8f);
    if (g_tabfade < 1.0f) g_tabfade = fminf(1.0f, g_tabfade + g_dt / 0.28f);

    clamp_tab(g_tab);
    g_scroll[g_tab] += ((float)g_top[g_tab] - g_scroll[g_tab]) * SMOOTH(16.0f);
    g_hl[g_tab] += ((float)g_sel[g_tab] - g_hl[g_tab]) * SMOOTH(22.0f);

    for (int i = 0; i < FLY_COUNT; i++) {
        g_flies[i].y -= g_flies[i].speed * g_dt;
        if (g_flies[i].y < -12.0f) g_flies[i].y += SCREEN_H + 24.0f;
    }
}

int ui_init(void) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    vita2d_init();
    vita2d_set_clear_color(COL_BG);
    vita2d_set_vblank_wait(1);

    vita2d_system_pgf_config full[] = {
        {SCE_FONT_LANGUAGE_KOREAN, in_hangul},
        {SCE_FONT_LANGUAGE_JAPANESE, in_cjk},
        {SCE_FONT_LANGUAGE_LATIN, NULL},
    };
    vita2d_system_pgf_config reduced[] = {
        {SCE_FONT_LANGUAGE_JAPANESE, in_cjk},
        {SCE_FONT_LANGUAGE_LATIN, NULL},
    };
    g_font = vita2d_load_system_pgf(3, full);
    if (!g_font) g_font = vita2d_load_system_pgf(2, reduced);
    if (!g_font) g_font = vita2d_load_default_pgf();
    if (!g_font) return -1;
    int h = vita2d_pgf_text_height(g_font, 1.0f, "Ag");
    if (h > 8 && h < 80) g_lh = (float)h;
    log_msg("font loaded, line height %.1f", g_lh);

    g_ps = gfx_font_load("app0:assets/PSButtons.ttf");
    g_glow = gfx_make_glow(128);
    g_logo = vita2d_load_PNG_file("app0:assets/Logo.png");
    vita2d_texture_set_filters(g_logo, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    flies_init();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    memset(&g_pad, 0, sizeof g_pad);
    memset(&g_prev, 0, sizeof g_prev);
    g_pad.ly = 128;
    g_pad.rx = 128;
    return 0;
}

int ui_frame(void) {
    g_prev = g_pad;
    sceCtrlPeekBufferPositive(0, &g_pad, 1);

    if (!g_started) {
        g_started = 1;
        if (!net_wifi_connected()) toast("Wi-Fi is not connected");
        else toast("Press SELECT to search");
    }

    uint32_t t = now_ms();
    pl_state_t st = player_state();
    if ((st == PL_PLAYING || st == PL_LOADING) && t - g_last_tick > 5000) {
        g_last_tick = t;
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    }

    if (pressed(SCE_CTRL_START)) set_screen_off(!g_screen_off);
    if (g_screen_off) {
        render_black();
        sleep_ms(30);
        return 0;
    }

    handle_buttons();
    handle_touch();
    update_art();
    animate();
    render_frame(0);
    return 0;
}

void ui_shutdown(void) {
    vita2d_wait_rendering_done();
    if (g_art) vita2d_free_texture(g_art);
    if (g_glow) vita2d_free_texture(g_glow);
    vita2d_free_texture(g_logo);
    gfx_font_free(g_ps);
    if (g_font) vita2d_free_pgf(g_font);
    vita2d_fini();
}
