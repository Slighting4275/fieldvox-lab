/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_ANDROID_TEXT_H
#define FV_ANDROID_TEXT_H
#include <stdbool.h>
#include <stdint.h>
/* Android sp sizes, in ascending order: 8, 11, 12, 14, 16, 20. */
#define FV_TEXT_SIZE_COUNT 6
typedef struct { float px[FV_TEXT_SIZE_COUNT]; } FvTextScale;
typedef struct { unsigned char *bitmap; int w,h,xoff,yoff; float advance; } FvTextGlyph;
void fv_android_text_set_vm(void *vm);
bool fv_android_text_available(void);
FvTextScale fv_android_text_scale(void *activity,float density);
float fv_text_sp(const FvTextScale *scale,float sp);
bool fv_android_text_glyph(int font,float pixels,uint32_t codepoint,FvTextGlyph *glyph);
bool fv_android_text_metrics(int font,float pixels,float *top,float *bottom);
float fv_android_text_kerning(int font,float pixels,uint32_t left,uint32_t right);
void fv_android_text_shutdown(void);
#endif
