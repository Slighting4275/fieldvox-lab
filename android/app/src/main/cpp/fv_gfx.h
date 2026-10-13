#ifndef FIELDVOX_GFX_H
#define FIELDVOX_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Small anti-aliased software rasteriser for the NativeActivity window (RGBA_8888 buffers).
 * Colours are 0xAARRGGBB. Shapes use signed-distance coverage; text uses stb_truetype with
 * platform sans-serif typefaces and em sizes, with a TrueType fallback.
 */

typedef struct {
    uint32_t *px;
    int w, h, stride;
    int clip_x0, clip_y0, clip_x1, clip_y1;
} FvCanvas;

enum { FVG_FONT_SANS = 0, FVG_FONT_MONO = 1, FVG_FONT_MEDIUM = 2, FVG_FONT_BOLD = 3, FVG_FONT_COUNT };

void fvg_canvas_init(FvCanvas *c, uint32_t *px, int w, int h, int stride);
void fvg_clip(FvCanvas *c, int x0, int y0, int x1, int y1);
void fvg_clip_reset(FvCanvas *c);

void fvg_clear(FvCanvas *c, uint32_t argb);
void fvg_fill_rect(FvCanvas *c, float x, float y, float w, float h, uint32_t argb);
/* Rounded rectangle with optional fill (alpha 0 = none) and inner stroke (width 0 = none). */
void fvg_round_rect(FvCanvas *c, float x, float y, float w, float h, float radius,
                    uint32_t fill, uint32_t stroke, float stroke_w);
void fvg_line(FvCanvas *c, float x0, float y0, float x1, float y1, float width, uint32_t argb);
void fvg_circle(FvCanvas *c, float cx, float cy, float r, uint32_t fill, uint32_t stroke, float stroke_w);
void fvg_triangle(FvCanvas *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb);
/* Bilinear scaled blit of an opaque 0xAARRGGBB image. */
void fvg_blit_scaled(FvCanvas *c, const uint32_t *src, int sw, int sh, float dx, float dy, float dw, float dh);
/* Aspect-ratio preserving center-crop blit (cover mode) that fills canvas without stretching. */
void fvg_blit_cover(FvCanvas *c, const uint32_t *src, int sw, int sh);

bool fvg_font_load(int font, const char *path);
bool fvg_font_ready(int font);
/* Call on the render thread after its final frame. */
void fvg_text_shutdown(void);
/* Draws UTF-8 text with its baseline at `y`; returns the advance width in pixels. */
float fvg_text(FvCanvas *c, int font, float size_px, float x, float y, const char *utf8, uint32_t argb, bool bold);
float fvg_text_width(int font, float size_px, const char *utf8, bool bold);
/* UTF-8-aware elision preserves the declared text size for long filenames. */
void fvg_text_elide(int font,float size_px,const char *utf8,float max_width,bool bold,char *out,size_t capacity);
void fvg_text_elide_filename(int font,float size_px,const char *name,float max_width,char *out,size_t capacity);
/* Centered in the box horizontally and vertically. */
void fvg_text_center(FvCanvas *c, int font, float size_px, float x, float y, float w, float h,
                     const char *utf8, uint32_t argb, bool bold);
/* Soft glow behind text (the Java setShadowLayer look). */
void fvg_text_glow(FvCanvas *c, int font, float size_px, float x, float y, const char *utf8,
                   uint32_t glow_argb, float radius, bool bold);

#endif
