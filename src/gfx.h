#ifndef GFX_UTIL_H
#define GFX_UTIL_H

#include <vita2d.h>

#define SCREEN_W 960.0f
#define SCREEN_H 544.0f

unsigned int gfx_color(int r, int g, int b, float a);
unsigned int gfx_lerp_color(unsigned int c0, unsigned int c1, float t);

float gfx_clamp01(float v);
float gfx_lerp(float a, float b, float t);
float gfx_ease_out_cubic(float t);

void gfx_rounded_rect(float x, float y, float w, float h, float r, unsigned int color);
void gfx_rounded_rect_vgrad(float x, float y, float w, float h, float r, unsigned int top, unsigned int bottom);

void gfx_rounded_ring(float x, float y, float w, float h, float r, float t, unsigned int color);
vita2d_texture *gfx_make_glow(int size);
void gfx_draw_glow(const vita2d_texture *tex, float cx, float cy, float radius, int r, int g, int b, float alpha);
void gfx_draw_glow_ellipse(const vita2d_texture *tex, float cx, float cy, float rx, float ry, int r, int g, int b, float alpha);

#endif
