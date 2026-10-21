#ifndef FONT_H
#define FONT_H

typedef struct GfxFont GfxFont;

GfxFont *gfx_font_load(const char *path);
void gfx_font_free(GfxFont *font);
int gfx_font_width(GfxFont *font, int size, const char *s);
void gfx_font_draw(GfxFont *font, float x, float baseline, int size, unsigned int color, const char *s);
void gfx_font_draw_icon(GfxFont *font, float x, float cy, int size, unsigned int color, const char *s);

#endif
