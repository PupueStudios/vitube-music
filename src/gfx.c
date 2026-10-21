#include <stddef.h>
#include <math.h>
#include "gfx.h"

float gfx_clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

float gfx_lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

float gfx_ease_out_cubic(float t)
{
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

unsigned int gfx_color(int r, int g, int b, float a)
{
    int ai = (int)(gfx_clamp01(a) * 255.0f + 0.5f);
    return RGBA8(r, g, b, ai);
}

unsigned int gfx_lerp_color(unsigned int c0, unsigned int c1, float t)
{
    t = gfx_clamp01(t);
    int r = (int)gfx_lerp((float)(c0 & 0xFF), (float)(c1 & 0xFF), t);
    int g = (int)gfx_lerp((float)((c0 >> 8) & 0xFF), (float)((c1 >> 8) & 0xFF), t);
    int b = (int)gfx_lerp((float)((c0 >> 16) & 0xFF), (float)((c1 >> 16) & 0xFF), t);
    int a = (int)gfx_lerp((float)((c0 >> 24) & 0xFF), (float)((c1 >> 24) & 0xFF), t);
    return RGBA8(r, g, b, a);
}

static float corner_inset(float r, float mid)
{
    float dy = r - mid;
    float v = r * r - dy * dy;
    if (v < 0.0f) v = 0.0f;
    return r - sqrtf(v);
}

void gfx_rounded_rect_vgrad(float x, float y, float w, float h, float r, unsigned int top, unsigned int bottom)
{
    if (w <= 0.0f || h <= 0.0f) return;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (r < 0.0f) r = 0.0f;

    int solid = (top == bottom);
    int ri = (int)(r + 0.5f);
    int step = ri > 18 ? 2 : 1;

    for (int i = 0; i < ri; i += step) {
        int hgt = (i + step <= ri) ? step : ri - i;
        float mid = (float)i + (float)hgt * 0.5f;
        float inset = corner_inset((float)ri, mid);
        float rw = w - inset * 2.0f;
        if (rw <= 0.0f) continue;
        unsigned int ct = solid ? top : gfx_lerp_color(top, bottom, mid / h);
        unsigned int cb = solid ? top : gfx_lerp_color(top, bottom, 1.0f - mid / h);
        vita2d_draw_rectangle(x + inset, y + (float)i, rw, (float)hgt, ct);
        vita2d_draw_rectangle(x + inset, y + h - (float)i - (float)hgt, rw, (float)hgt, cb);
    }

    float my = y + (float)ri;
    float mh = h - 2.0f * (float)ri;
    if (mh <= 0.0f) return;

    if (solid) {
        vita2d_draw_rectangle(x, my, w, mh, top);
        return;
    }

    const float band = 8.0f;
    for (float yy = 0.0f; yy < mh; yy += band) {
        float bh = fminf(band, mh - yy);
        float t = ((float)ri + yy + bh * 0.5f) / h;
        vita2d_draw_rectangle(x, my + yy, w, bh, gfx_lerp_color(top, bottom, t));
    }
}

void gfx_rounded_rect(float x, float y, float w, float h, float r, unsigned int color)
{
    gfx_rounded_rect_vgrad(x, y, w, h, r, color, color);
}

void gfx_rounded_ring(float x, float y, float w, float h, float r, float t, unsigned int color)
{
    if (w <= 0.0f || h <= 0.0f || t <= 0.0f) return;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (t > r) t = r;

    int ri = (int)(r + 0.5f);
    float rin = (float)ri - t;

    for (int i = 0; i < ri; i++) {
        float mid = (float)i + 0.5f;
        float o = corner_inset((float)ri, mid);
        float ow = w - o * 2.0f;
        if (ow <= 0.0f) continue;

        if ((float)i < t || rin <= 0.0f) {
            vita2d_draw_rectangle(x + o, y + (float)i, ow, 1.0f, color);
            vita2d_draw_rectangle(x + o, y + h - 1.0f - (float)i, ow, 1.0f, color);
        } else {
            float mi = mid - t;
            float oi = t + corner_inset(rin, mi);
            float sw = oi - o;
            if (sw <= 0.0f) continue;
            vita2d_draw_rectangle(x + o, y + (float)i, sw, 1.0f, color);
            vita2d_draw_rectangle(x + w - oi, y + (float)i, sw, 1.0f, color);
            vita2d_draw_rectangle(x + o, y + h - 1.0f - (float)i, sw, 1.0f, color);
            vita2d_draw_rectangle(x + w - oi, y + h - 1.0f - (float)i, sw, 1.0f, color);
        }
    }

    float mh = h - 2.0f * (float)ri;
    if (mh > 0.0f) {
        vita2d_draw_rectangle(x, y + (float)ri, t, mh, color);
        vita2d_draw_rectangle(x + w - t, y + (float)ri, t, mh, color);
    }
}

vita2d_texture *gfx_make_glow(int size)
{
    vita2d_texture *tex = vita2d_create_empty_texture((unsigned int)size, (unsigned int)size);
    if (!tex) return NULL;

    unsigned int *px = (unsigned int *)vita2d_texture_get_datap(tex);
    int stride = (int)vita2d_texture_get_stride(tex) / 4;
    float c = (float)(size - 1) * 0.5f;

    for (int yy = 0; yy < size; yy++) {
        for (int xx = 0; xx < size; xx++) {
            float dx = ((float)xx - c) / c;
            float dy = ((float)yy - c) / c;
            float d = sqrtf(dx * dx + dy * dy);
            float f = d >= 1.0f ? 0.0f : 1.0f - d;
            px[yy * stride + xx] = RGBA8(255, 255, 255, (int)(f * f * 255.0f + 0.5f));
        }
    }

    vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return tex;
}

void gfx_draw_glow_ellipse(const vita2d_texture *tex, float cx, float cy, float rx, float ry, int r, int g, int b, float alpha)
{
    if (!tex || alpha <= 0.0f) return;
    float tw = (float)vita2d_texture_get_width(tex);
    float th = (float)vita2d_texture_get_height(tex);
    vita2d_draw_texture_tint_scale(tex, cx - rx, cy - ry, rx * 2.0f / tw, ry * 2.0f / th, gfx_color(r, g, b, alpha));
}

void gfx_draw_glow(const vita2d_texture *tex, float cx, float cy, float radius, int r, int g, int b, float alpha)
{
    gfx_draw_glow_ellipse(tex, cx, cy, radius, radius, r, g, b, alpha);
}
