#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <vita2d.h>
#include "font.h"

#define FIRST_CHAR 32
#define CHAR_COUNT 95
#define ATLAS_W 512
#define PAD 1
#define MAX_SIZES 8

typedef struct {
    short x, y, w, h;
    short left, top;
    float adv;
} Glyph;

typedef struct {
    int size;
    vita2d_texture *tex;
    Glyph g[CHAR_COUNT];
} Atlas;

struct GfxFont {
    FT_Face face;
    int count;
    Atlas atlas[MAX_SIZES];
};

static FT_Library g_lib;
static int g_users;

static void lib_release(void)
{
    if (g_users == 0 && g_lib) {
        FT_Done_FreeType(g_lib);
        g_lib = NULL;
    }
}

GfxFont *gfx_font_load(const char *path)
{
    if (!g_lib && FT_Init_FreeType(&g_lib)) {
        g_lib = NULL;
        return NULL;
    }

    GfxFont *f = (GfxFont *)calloc(1, sizeof(*f));
    if (!f) {
        lib_release();
        return NULL;
    }

    if (FT_New_Face(g_lib, path, 0, &f->face)) {
        free(f);
        lib_release();
        return NULL;
    }

    g_users++;
    return f;
}

void gfx_font_free(GfxFont *f)
{
    if (!f) return;
    for (int i = 0; i < f->count; i++) {
        if (f->atlas[i].tex) vita2d_free_texture(f->atlas[i].tex);
    }
    FT_Done_Face(f->face);
    free(f);
    g_users--;
    lib_release();
}

static int atlas_build(GfxFont *f, Atlas *a, int size)
{
    if (FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)size)) return 0;

    const FT_Int32 flags = FT_LOAD_RENDER | FT_LOAD_NO_HINTING;
    int px = PAD;
    int py = PAD;
    int row_h = 0;

    for (int i = 0; i < CHAR_COUNT; i++) {
        Glyph *g = &a->g[i];
        memset(g, 0, sizeof(*g));
        if (FT_Load_Char(f->face, (FT_ULong)(FIRST_CHAR + i), flags)) continue;

        FT_GlyphSlot s = f->face->glyph;
        g->adv = (float)s->advance.x / 64.0f;
        g->w = (short)s->bitmap.width;
        g->h = (short)s->bitmap.rows;
        g->left = (short)s->bitmap_left;
        g->top = (short)s->bitmap_top;
        if (g->w <= 0 || g->h <= 0 || g->w + PAD * 2 > ATLAS_W) {
            g->w = 0;
            g->h = 0;
            continue;
        }

        if (px + g->w + PAD > ATLAS_W) {
            px = PAD;
            py += row_h + PAD;
            row_h = 0;
        }
        g->x = (short)px;
        g->y = (short)py;
        px += g->w + PAD;
        if (g->h > row_h) row_h = g->h;
    }

    int height = py + row_h + PAD;
    a->tex = vita2d_create_empty_texture(ATLAS_W, (unsigned int)height);
    if (!a->tex) return 0;

    unsigned int *dst = (unsigned int *)vita2d_texture_get_datap(a->tex);
    int stride = (int)vita2d_texture_get_stride(a->tex) / 4;
    memset(dst, 0, (size_t)stride * (size_t)height * 4u);

    for (int i = 0; i < CHAR_COUNT; i++) {
        Glyph *g = &a->g[i];
        if (g->w <= 0) continue;
        if (FT_Load_Char(f->face, (FT_ULong)(FIRST_CHAR + i), flags)) {
            g->w = 0;
            g->h = 0;
            continue;
        }

        FT_Bitmap *bm = &f->face->glyph->bitmap;
        if (bm->pixel_mode != FT_PIXEL_MODE_GRAY || (int)bm->width != g->w || (int)bm->rows != g->h) {
            g->w = 0;
            g->h = 0;
            continue;
        }

        for (int r = 0; r < g->h; r++) {
            const unsigned char *src = bm->buffer + (long)r * (long)bm->pitch;
            unsigned int *row = dst + (g->y + r) * stride + g->x;
            for (int c = 0; c < g->w; c++) row[c] = RGBA8(255, 255, 255, src[c]);
        }
    }

    vita2d_texture_set_filters(a->tex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    return 1;
}

static Atlas *atlas_get(GfxFont *f, int size)
{
    if (!f || size <= 0) return NULL;
    for (int i = 0; i < f->count; i++) {
        if (f->atlas[i].size == size) return &f->atlas[i];
    }
    if (f->count >= MAX_SIZES) return NULL;

    Atlas *a = &f->atlas[f->count];
    memset(a, 0, sizeof(*a));
    a->size = size;
    if (!atlas_build(f, a, size)) {
        if (a->tex) vita2d_free_texture(a->tex);
        memset(a, 0, sizeof(*a));
        return NULL;
    }
    f->count++;
    return a;
}

int gfx_font_width(GfxFont *f, int size, const char *s)
{
    Atlas *a = atlas_get(f, size);
    if (!a || !s) return 0;

    float w = 0.0f;
    for (; *s; s++) {
        int c = (unsigned char)*s - FIRST_CHAR;
        if (c < 0 || c >= CHAR_COUNT) continue;
        w += a->g[c].adv;
    }
    return (int)(w + 0.5f);
}

static void draw_run(GfxFont *f, float x, float y, int size, unsigned int color, const char *s, int centered)
{
    Atlas *a = atlas_get(f, size);
    if (!a || !s) return;

    float pen = x;
    for (; *s; s++) {
        int c = (unsigned char)*s - FIRST_CHAR;
        if (c < 0 || c >= CHAR_COUNT) continue;

        const Glyph *g = &a->g[c];
        if (g->w > 0) {
            float base = centered ? y + (float)g->top - (float)g->h * 0.5f : y;
            float gx = floorf(pen + 0.5f) + (float)g->left;
            float gy = floorf(base + 0.5f) - (float)g->top;
            vita2d_draw_texture_tint_part_scale(a->tex, gx, gy, (float)g->x, (float)g->y,
                                                (float)g->w, (float)g->h, 1.0f, 1.0f, color);
        }
        pen += g->adv;
    }
}

void gfx_font_draw(GfxFont *f, float x, float baseline, int size, unsigned int color, const char *s)
{
    draw_run(f, x, baseline, size, color, s, 0);
}

void gfx_font_draw_icon(GfxFont *f, float x, float cy, int size, unsigned int color, const char *s)
{
    draw_run(f, x, cy, size, color, s, 1);
}
