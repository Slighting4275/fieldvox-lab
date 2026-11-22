/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
/*
 * FieldVox anti-aliased software rasteriser. See fv_gfx.h.
 */

#include "fv_gfx.h"
#include "fv_android_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"

/* ---------- pixels ---------- */

static inline uint32_t to_native(uint32_t argb) {
    /* RGBA_8888 in memory is R,G,B,A: swap the R and B channels of 0xAARRGGBB. */
    return (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline void blend(uint32_t *dst, uint32_t argb, float coverage) {
    const float a = (float)(argb >> 24) / 255.0f * coverage;
    if (a <= 0.002f) return;
    if (a >= 0.998f) {
        *dst = to_native(argb | 0xFF000000u);
        return;
    }
    const uint32_t d = *dst;
    const float sr = (float)((argb >> 16) & 0xFFu), sg = (float)((argb >> 8) & 0xFFu), sb = (float)(argb & 0xFFu);
    const float dr = (float)(d & 0xFFu), dg = (float)((d >> 8) & 0xFFu), db = (float)((d >> 16) & 0xFFu);
    const uint32_t r = (uint32_t)(dr + (sr - dr) * a + 0.5f);
    const uint32_t g = (uint32_t)(dg + (sg - dg) * a + 0.5f);
    const uint32_t b = (uint32_t)(db + (sb - db) * a + 0.5f);
    *dst = 0xFF000000u | (b << 16) | (g << 8) | r;
}

void fvg_canvas_init(FvCanvas *c, uint32_t *px, int w, int h, int stride) {
    c->px = px;
    c->w = w;
    c->h = h;
    c->stride = stride;
    fvg_clip_reset(c);
}

void fvg_clip(FvCanvas *c, int x0, int y0, int x1, int y1) {
    c->clip_x0 = x0 < 0 ? 0 : x0;
    c->clip_y0 = y0 < 0 ? 0 : y0;
    c->clip_x1 = x1 > c->w ? c->w : x1;
    c->clip_y1 = y1 > c->h ? c->h : y1;
}

void fvg_clip_reset(FvCanvas *c) {
    c->clip_x0 = 0;
    c->clip_y0 = 0;
    c->clip_x1 = c->w;
    c->clip_y1 = c->h;
}

/* Clamps a float box (with `pad` pixels of AA margin) to the clip; false when empty. */
static bool span(const FvCanvas *c, float x0, float y0, float x1, float y1, float pad,
                 int *ix0, int *iy0, int *ix1, int *iy1) {
    int a = (int)floorf(x0 - pad), b = (int)floorf(y0 - pad);
    int e = (int)ceilf(x1 + pad), f = (int)ceilf(y1 + pad);
    if (a < c->clip_x0) a = c->clip_x0;
    if (b < c->clip_y0) b = c->clip_y0;
    if (e > c->clip_x1) e = c->clip_x1;
    if (f > c->clip_y1) f = c->clip_y1;
    *ix0 = a;
    *iy0 = b;
    *ix1 = e;
    *iy1 = f;
    return a < e && b < f;
}

void fvg_clear(FvCanvas *c, uint32_t argb) {
    const uint32_t n = to_native(argb | 0xFF000000u);
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = c->px + (size_t)y * (size_t)c->stride;
        for (int x = 0; x < c->w; x++) row[x] = n;
    }
}

void fvg_fill_rect(FvCanvas *c, float x, float y, float w, float h, uint32_t argb) {
    int x0, y0, x1, y1;
    if (w <= 0 || h <= 0 || !span(c, x, y, x + w, y + h, 0.0f, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++) {
        const float cy = clampf(fminf((float)py + 1.0f, y + h) - fmaxf((float)py, y), 0.0f, 1.0f);
        uint32_t *row = c->px + (size_t)py * (size_t)c->stride;
        for (int px = x0; px < x1; px++) {
            const float cx = clampf(fminf((float)px + 1.0f, x + w) - fmaxf((float)px, x), 0.0f, 1.0f);
            blend(&row[px], argb, cx * cy);
        }
    }
}

/* Signed distance from p to a rounded box centred at the origin (negative inside). */
static inline float sd_round_box(float px, float py, float hx, float hy, float r) {
    const float qx = fabsf(px) - (hx - r), qy = fabsf(py) - (hy - r);
    const float ox = fmaxf(qx, 0.0f), oy = fmaxf(qy, 0.0f);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
}

void fvg_round_rect(FvCanvas *c, float x, float y, float w, float h, float radius,
                    uint32_t fill, uint32_t stroke, float stroke_w) {
    if (w <= 0 || h <= 0) return;
    const float hx = w * 0.5f, hy = h * 0.5f;
    const float r = clampf(radius, 0.0f, fminf(hx, hy));
    const float cx = x + hx, cy = y + hy;
    const bool has_fill = (fill >> 24) != 0;
    const bool has_stroke = (stroke >> 24) != 0 && stroke_w > 0.0f;
    const float band = r + (has_stroke ? stroke_w : 0.0f) + 2.0f; /* rows/cols needing exact coverage */
    int x0, y0, x1, y1;
    if (!span(c, x, y, x + w, y + h, 1.0f, &x0, &y0, &x1, &y1)) return;

    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * (size_t)c->stride;
        const float fy = (float)py + 0.5f - cy;
        const bool corner_row = fabsf(fy) > hy - band;
        for (int px = x0; px < x1; px++) {
            const float fx = (float)px + 0.5f - cx;
            if (!corner_row && fabsf(fx) < hx - band) {
                /* Interior fast path: full fill, no stroke this far from the edge. */
                if (has_fill) blend(&row[px], fill, 1.0f);
                continue;
            }
            const float d = sd_round_box(fx, fy, hx, hy, r);
            if (has_fill) {
                const float cov = clampf(0.5f - d, 0.0f, 1.0f);
                if (cov > 0.0f) blend(&row[px], fill, cov);
            }
            if (has_stroke) {
                const float ring = fabsf(d + stroke_w * 0.5f) - stroke_w * 0.5f;
                const float cov = clampf(0.5f - ring, 0.0f, 1.0f);
                if (cov > 0.0f) blend(&row[px], stroke, cov);
            }
        }
    }
}

void fvg_line(FvCanvas *c, float ax, float ay, float bx, float by, float width, uint32_t argb) {
    const float hw = width * 0.5f;
    int x0, y0, x1, y1;
    if (!span(c, fminf(ax, bx), fminf(ay, by), fmaxf(ax, bx), fmaxf(ay, by), hw + 1.0f, &x0, &y0, &x1, &y1)) return;
    const float bax = bx - ax, bay = by - ay;
    const float len2 = bax * bax + bay * bay;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * (size_t)c->stride;
        for (int px = x0; px < x1; px++) {
            const float pax = (float)px + 0.5f - ax, pay = (float)py + 0.5f - ay;
            const float t = len2 > 0.0f ? clampf((pax * bax + pay * bay) / len2, 0.0f, 1.0f) : 0.0f;
            const float dx = pax - bax * t, dy = pay - bay * t;
            const float d = sqrtf(dx * dx + dy * dy) - hw;
            const float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) blend(&row[px], argb, cov);
        }
    }
}

void fvg_circle(FvCanvas *c, float cx, float cy, float r, uint32_t fill, uint32_t stroke, float stroke_w) {
    int x0, y0, x1, y1;
    if (!span(c, cx - r, cy - r, cx + r, cy + r, 1.0f + stroke_w, &x0, &y0, &x1, &y1)) return;
    const bool has_fill = (fill >> 24) != 0;
    const bool has_stroke = (stroke >> 24) != 0 && stroke_w > 0.0f;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * (size_t)c->stride;
        for (int px = x0; px < x1; px++) {
            const float dx = (float)px + 0.5f - cx, dy = (float)py + 0.5f - cy;
            const float d = sqrtf(dx * dx + dy * dy) - r;
            if (has_fill) blend(&row[px], fill, clampf(0.5f - d, 0.0f, 1.0f));
            if (has_stroke) {
                /* Centred stroke, like Paint.Style.STROKE. */
                const float ring = fabsf(d) - stroke_w * 0.5f;
                blend(&row[px], stroke, clampf(0.5f - ring, 0.0f, 1.0f));
            }
        }
    }
}

static inline float edge_dist(float px, float py, float ax, float ay, float bx, float by) {
    const float ex = bx - ax, ey = by - ay;
    const float len = sqrtf(ex * ex + ey * ey);
    return len > 0.0f ? ((px - ax) * ey - (py - ay) * ex) / len : 0.0f;
}

void fvg_triangle(FvCanvas *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb) {
    /* Make the winding consistent so "inside" is a positive distance to every edge. */
    const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area > 0.0f) {
        float tx = x1, ty = y1;
        x1 = x2;
        y1 = y2;
        x2 = tx;
        y2 = ty;
    }
    int ix0, iy0, ix1, iy1;
    if (!span(c, fminf(x0, fminf(x1, x2)), fminf(y0, fminf(y1, y2)), fmaxf(x0, fmaxf(x1, x2)),
              fmaxf(y0, fmaxf(y1, y2)), 1.0f, &ix0, &iy0, &ix1, &iy1)) return;
    for (int py = iy0; py < iy1; py++) {
        uint32_t *row = c->px + (size_t)py * (size_t)c->stride;
        for (int px = ix0; px < ix1; px++) {
            const float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
            const float d = fminf(edge_dist(fx, fy, x0, y0, x1, y1),
                                  fminf(edge_dist(fx, fy, x1, y1, x2, y2), edge_dist(fx, fy, x2, y2, x0, y0)));
            const float cov = clampf(d + 0.5f, 0.0f, 1.0f);
            if (cov > 0.0f) blend(&row[px], argb, cov);
        }
    }
}

void fvg_blit_scaled(FvCanvas *c, const uint32_t *src, int sw, int sh, float dx, float dy, float dw, float dh) {
    int x0, y0, x1, y1;
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        !span(c, dx, dy, dx + dw, dy + dh, 0.0f, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * (size_t)c->stride;
        float sy = ((float)py + 0.5f - dy) / dh * (float)sh - 0.5f;
        sy = clampf(sy, 0.0f, (float)(sh - 1));
        const int yA = (int)sy, yB = yA + 1 < sh ? yA + 1 : yA;
        const float ty = sy - (float)yA;
        for (int px = x0; px < x1; px++) {
            float sx = ((float)px + 0.5f - dx) / dw * (float)sw - 0.5f;
            sx = clampf(sx, 0.0f, (float)(sw - 1));
            const int xA = (int)sx, xB = xA + 1 < sw ? xA + 1 : xA;
            const float tx = sx - (float)xA;
            const uint32_t p00 = src[yA * sw + xA], p01 = src[yA * sw + xB];
            const uint32_t p10 = src[yB * sw + xA], p11 = src[yB * sw + xB];
            uint32_t out = 0xFF000000u;
            for (int shift = 0; shift <= 16; shift += 8) {
                const float a = (float)((p00 >> shift) & 0xFFu) * (1.0f - tx) + (float)((p01 >> shift) & 0xFFu) * tx;
                const float b = (float)((p10 >> shift) & 0xFFu) * (1.0f - tx) + (float)((p11 >> shift) & 0xFFu) * tx;
                out |= (uint32_t)(a * (1.0f - ty) + b * ty + 0.5f) << shift;
            }
            row[px] = to_native(out);
        }
    }
}

void fvg_blit_cover(FvCanvas *c, const uint32_t *src, int sw, int sh) {
    if (!c || !src || sw <= 0 || sh <= 0 || c->w <= 0 || c->h <= 0) return;

    /* Cover mode: scale so that src completely fills canvas with preserved aspect ratio */
    const float scale = fmaxf((float)c->w / (float)sw, (float)c->h / (float)sh);
    const float dw = (float)sw * scale;
    const float dh = (float)sh * scale;
    const float dx = ((float)c->w - dw) * 0.5f;
    const float dy = ((float)c->h - dh) * 0.5f;

    fvg_blit_scaled(c, src, sw, sh, dx, dy, dw, dh);
}

/* ---------- text ---------- */

typedef struct {
    unsigned char *data;
    stbtt_fontinfo info;
    bool ready;
} FvFont;

typedef struct {
    bool used;
    int font;
    uint32_t size_key;
    uint32_t cp;
    int w, h, xoff, yoff;
    float advance;
    unsigned char *bmp;
    uint64_t last_used;
} FvGlyph;

#define GLYPH_CACHE_SIZE 4096

static FvFont g_fonts[FVG_FONT_COUNT];
static FvGlyph g_glyphs[GLYPH_CACHE_SIZE];
static uint64_t glyph_age;
typedef struct { bool used; int font; uint32_t size_key; uint32_t left,right; float value; } FvKern;
static FvKern g_kerns[2048];
void fvg_text_shutdown(void) {
    fv_android_text_shutdown();
    for (int i=0;i<GLYPH_CACHE_SIZE;i++) free(g_glyphs[i].bmp);
    memset(g_glyphs,0,sizeof(g_glyphs)); memset(g_kerns,0,sizeof(g_kerns)); glyph_age=0;
    for (int i=0;i<FVG_FONT_COUNT;i++) { free(g_fonts[i].data); g_fonts[i]=(FvFont){0}; }
}

bool fvg_font_load(int font, const char *path) {
    if (font < 0 || font >= FVG_FONT_COUNT || !path) return false;
    if (g_fonts[font].ready) return true;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    const long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    unsigned char *data = (unsigned char *)malloc((size_t)size);
    const bool read_ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!read_ok || !stbtt_InitFont(&g_fonts[font].info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        free(data);
        return false;
    }
    g_fonts[font].data = data;
    g_fonts[font].ready = true;
    return true;
}

bool fvg_font_ready(int font) {
    return font >= 0 && font < FVG_FONT_COUNT && (g_fonts[font].ready || fv_android_text_available());
}

static uint32_t next_codepoint(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int extra;
    if (p[0] < 0x80) {
        cp = p[0];
        extra = 0;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1Fu;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0Fu;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07u;
        extra = 3;
    } else {
        *s += 1;
        return '?';
    }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += i;
            return '?';
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    *s += 1 + extra;
    return cp;
}

static const FvGlyph *get_glyph(int font, float size_px, uint32_t cp) {
    if (!isfinite(size_px) || size_px<=0 || size_px>512) return NULL;
    uint32_t size_key; memcpy(&size_key,&size_px,sizeof(size_key));
    const uint32_t hash = (cp * 2654435761u) ^ ((size_key^(size_key>>16u)) * 40503u) ^ ((uint32_t)font * 97u);
    FvGlyph *slot=NULL;
    for (int probe = 0; probe < 32; probe++) {
        FvGlyph *g = &g_glyphs[(hash + (uint32_t)probe) % GLYPH_CACHE_SIZE];
        if (g->used && g->font == font && g->size_key == size_key && g->cp == cp) { g->last_used=++glyph_age; return g; }
        if (!g->used) { slot=g; break; }
        if (!slot || g->last_used<slot->last_used) slot=g;
    }
    /* Reclaim the oldest neighbour instead of silently dropping text when
     * font-size changes or long recordings fill this bounded cache. */
    if (!slot) return NULL;
    FvTextGlyph raster={0};
    const float pixels=size_px;
    if (!fv_android_text_glyph(font,pixels,cp,&raster)) {
        if (!g_fonts[font].ready) return NULL;
        const stbtt_fontinfo *info=&g_fonts[font].info;
        const float scale=stbtt_ScaleForMappingEmToPixels(info,pixels);
        int advance=0,lsb=0; stbtt_GetCodepointHMetrics(info,(int)cp,&advance,&lsb);
        raster.bitmap=stbtt_GetCodepointBitmap(info,0,scale,(int)cp,&raster.w,&raster.h,&raster.xoff,&raster.yoff);
        raster.advance=(float)advance*scale;
        if (raster.w>0 && raster.h>0 && !raster.bitmap) return NULL;
    }
    free(slot->bmp);
    *slot=(FvGlyph){.used=true,.font=font,.size_key=size_key,.cp=cp,.w=raster.w,.h=raster.h,
        .xoff=raster.xoff,.yoff=raster.yoff,.advance=raster.advance,.bmp=raster.bitmap,.last_used=++glyph_age};
    return slot;
}
static float kerning(int font,float pixels,uint32_t left,uint32_t right) {
    uint32_t size_key; memcpy(&size_key,&pixels,sizeof(size_key));
    const unsigned hash=(left*2654435761u)^(right*40503u)^((size_key^(size_key>>16u))*97u)^(unsigned)font;
    FvKern *k=&g_kerns[hash%2048u];
    if (k->used && k->font==font && k->size_key==size_key && k->left==left && k->right==right) return k->value;
    float value;
    if (fv_android_text_available()) value=fv_android_text_kerning(font,pixels,left,right);
    else if (g_fonts[font].ready) value=(float)stbtt_GetCodepointKernAdvance(&g_fonts[font].info,(int)left,(int)right)*
        stbtt_ScaleForMappingEmToPixels(&g_fonts[font].info,pixels);
    else value=0;
    *k=(FvKern){.used=true,.font=font,.size_key=size_key,.left=left,.right=right,.value=value}; return value;
}
/* Glyphs Roboto lacks, drawn as shapes so the Java labels ("▶ FWD", "◀ REV") still render. */
static bool is_arrow(uint32_t cp) {
    return cp == 0x25B6u || cp == 0x25C0u;
}

static float draw_arrow(FvCanvas *c, float size_px, float x, float y, uint32_t cp, uint32_t argb, bool draw) {
    const float s = size_px * 0.62f;
    if (draw) {
        const float top = y - s * 0.95f, bottom = y - s * 0.05f, mid = (top + bottom) * 0.5f;
        if (cp == 0x25B6u) fvg_triangle(c, x, top, x, bottom, x + s * 0.85f, mid, argb);
        else fvg_triangle(c, x + s * 0.85f, top, x + s * 0.85f, bottom, x, mid, argb);
    }
    return s * 1.1f;
}

static float text_impl(FvCanvas *c, int font, float size_px, float x, float y, const char *utf8,
                       uint32_t argb, bool bold, bool draw) {
    if (!utf8 || !isfinite(size_px) || size_px<=0 || size_px>512 || !fvg_font_ready(font)) return 0.0f;
    if (bold && font==FVG_FONT_SANS && fvg_font_ready(FVG_FONT_BOLD)) { font=FVG_FONT_BOLD; bold=false; }
    const float embolden = bold ? fmaxf(1.0f, size_px / 30.0f) : 0.0f;
    float pen = x;
    const char *s = utf8; uint32_t previous=0;
    while (*s) {
        const uint32_t cp = next_codepoint(&s);
        if (previous) pen+=kerning(font,size_px,previous,cp);
        previous=cp;
        if (!fv_android_text_available() && g_fonts[font].ready && is_arrow(cp) && stbtt_FindGlyphIndex(&g_fonts[font].info, (int)cp) == 0) {
            pen += draw_arrow(c, size_px, pen, y, cp, argb, draw);
            continue;
        }
        const FvGlyph *g = get_glyph(font, size_px, cp);
        if (!g) continue;
        if (draw && g->bmp) {
            for (int pass = 0; pass <= (bold ? 1 : 0); pass++) {
                const int gx = (int)floorf(pen + (float)g->xoff + (pass ? embolden : 0.0f) + 0.5f);
                const int gy = (int)floorf(y + (float)g->yoff + 0.5f);
                for (int row = 0; row < g->h; row++) {
                    const int py = gy + row;
                    if (py < c->clip_y0 || py >= c->clip_y1) continue;
                    uint32_t *dst = c->px + (size_t)py * (size_t)c->stride;
                    for (int col = 0; col < g->w; col++) {
                        const int px = gx + col;
                        if (px < c->clip_x0 || px >= c->clip_x1) continue;
                        const unsigned char cov = g->bmp[row * g->w + col];
                        if (cov) blend(&dst[px], argb, (float)cov / 255.0f);
                    }
                }
            }
        }
        pen += g->advance + embolden;
    }
    return pen - x;
}

float fvg_text(FvCanvas *c, int font, float size_px, float x, float y, const char *utf8, uint32_t argb, bool bold) {
    return text_impl(c, font, size_px, x, y, utf8, argb, bold, true);
}

float fvg_text_width(int font, float size_px, const char *utf8, bool bold) {
    return text_impl(NULL, font, size_px, 0.0f, 0.0f, utf8, 0, bold, false);
}

void fvg_text_elide(int font,float size_px,const char *utf8,float max_width,bool bold,char *out,size_t capacity) {
    if (!out || !capacity) return;
    out[0]=0;
    if (!utf8 || !isfinite(max_width) || max_width<=0 || capacity<4u) return;
    size_t length=strlen(utf8); bool shortened=length>=capacity;
    if (length>=capacity) {
        length=capacity-4u;
        while (length && (((unsigned char)utf8[length]&0xc0u)==0x80u)) length--;
    }
    memcpy(out,utf8,length); out[length]=0;
    if (!shortened && fvg_text_width(font,size_px,out,bold)<=max_width) return;
    if (length>capacity-4u) length=capacity-4u;
    while (length && (((unsigned char)utf8[length]&0xc0u)==0x80u)) length--;
    for (;;) {
        memcpy(out+length,"…",4);
        if (fvg_text_width(font,size_px,out,bold)<=max_width) return;
        if (!length) { out[0]=0; return; }
        length--;
        while (length && (((unsigned char)utf8[length]&0xc0u)==0x80u)) length--;
    }
}

void fvg_text_elide_filename(int font,float size_px,const char *name,float max_width,char *out,size_t capacity) {
    if (!out || !capacity) return;
    out[0]=0;
    if (!name) return;
    const size_t length=strlen(name);
    if (length<capacity && fvg_text_width(font,size_px,name,false)<=max_width) {
        memcpy(out,name,length+1u); return;
    }
    const char *suffix=strrchr(name,'-');
    if (!suffix || strlen(suffix)>16u) suffix=strrchr(name,'.');
    if (!suffix || suffix==name || strlen(suffix)+4u>=capacity) {
        fvg_text_elide(font,size_px,name,max_width,false,out,capacity); return;
    }
    const size_t tail=strlen(suffix);
    size_t prefix=(size_t)(suffix-name);
    if (prefix>capacity-tail-4u) prefix=capacity-tail-4u;
    while (prefix && (((unsigned char)name[prefix]&0xc0u)==0x80u)) prefix--;
    for (;;) {
        memcpy(out,name,prefix); memcpy(out+prefix,"…",3); memcpy(out+prefix+3u,suffix,tail+1u);
        if (fvg_text_width(font,size_px,out,false)<=max_width) return;
        if (!prefix) { fvg_text_elide(font,size_px,name,max_width,false,out,capacity); return; }
        prefix--;
        while (prefix && (((unsigned char)name[prefix]&0xc0u)==0x80u)) prefix--;
    }
}

void fvg_text_center(FvCanvas *c, int font, float size_px, float x, float y, float w, float h,
                     const char *utf8, uint32_t argb, bool bold) {
    const float tw = fvg_text_width(font, size_px, utf8, bold);
    int face=font;
    if (bold && font==FVG_FONT_SANS && fvg_font_ready(FVG_FONT_BOLD)) face=FVG_FONT_BOLD;
    float top=0,bottom=0;
    if (!fv_android_text_metrics(face,size_px,&top,&bottom) && face>=0 && face<FVG_FONT_COUNT && g_fonts[face].ready) {
        int ascent=0,descent=0,gap=0; stbtt_GetFontVMetrics(&g_fonts[face].info,&ascent,&descent,&gap);
        const float scale=stbtt_ScaleForMappingEmToPixels(&g_fonts[face].info,size_px);
        top=-(float)ascent*scale; bottom=-(float)descent*scale;
    }
    const float baseline = y + h * 0.5f - (top+bottom)*0.5f;
    fvg_text(c, font, size_px, x + (w - tw) * 0.5f, baseline, utf8, argb, bold);
}

void fvg_text_glow(FvCanvas *c, int font, float size_px, float x, float y, const char *utf8,
                   uint32_t glow_argb, float radius, bool bold) {
    const uint32_t alpha = (glow_argb >> 24) / 14u; /* 16 taps accumulate: keep each faint */
    const uint32_t soft = (alpha << 24) | (glow_argb & 0x00FFFFFFu);
    for (int ring = 1; ring <= 2; ring++) {
        const float r = radius * (float)ring / 2.0f;
        for (int k = 0; k < 8; k++) {
            const float a = (float)k * 0.785398f;
            fvg_text(c, font, size_px, x + cosf(a) * r, y + sinf(a) * r, utf8, soft, bold);
        }
    }
}
