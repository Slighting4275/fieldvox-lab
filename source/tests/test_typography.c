/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "../../android/app/src/main/cpp/fv_gfx.h"
#include "../../android/app/src/main/cpp/fv_android_text.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill_split_image(uint32_t *buf, int w, int h, uint32_t left, uint32_t right) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            buf[(size_t)y * (size_t)w + (size_t)x] = (x < w / 2) ? left : right;
        }
    }
}

static void assert_cover_split(int sw, int sh, int dw, int dh) {
    const uint32_t argb_l = 0xFF11AA22u, argb_r = 0xFFAA44FFu;
    const uint32_t nat_l = 0xFF22AA11u, nat_r = 0xFFFF44AAu;
    uint32_t *src = malloc((size_t)sw * (size_t)sh * sizeof(*src));
    uint32_t *dst = calloc((size_t)dw * (size_t)dh, sizeof(*dst));
    assert(src && dst);
    fill_split_image(src, sw, sh, argb_l, argb_r);
    FvCanvas canvas;
    fvg_canvas_init(&canvas, dst, dw, dh, dw);
    fvg_blit_cover(&canvas, src, sw, sh);
    for (int y = 0; y < dh; y++) {
        assert(dst[(size_t)y * (size_t)dw] == nat_l);
        assert(dst[(size_t)y * (size_t)dw + (size_t)(dw - 1)] == nat_r);
        for (int x = 0; x < dw; x++) {
            const uint32_t px = dst[(size_t)y * (size_t)dw + (size_t)x];
            const uint32_t r = px & 0xFFu;
            const uint32_t g = (px >> 8) & 0xFFu;
            const uint32_t b = (px >> 16) & 0xFFu;
            assert((px >> 24) == 0xFFu);
            assert(r >= 0x11u && r <= 0xAAu);
            assert(g >= 0x44u && g <= 0xAAu);
            assert(b >= 0x22u && b <= 0xFFu);
        }
    }
    free(dst);
    free(src);
}

int main(int argc,char **argv) {
    if (argc!=2 || !fvg_font_load(FVG_FONT_SANS,argv[1])) {
        fputs("Typography test requires a readable TrueType font\n",stderr);
        return 1;
    }
    FvTextScale scale=fv_android_text_scale(NULL,3.0f);
    assert(fabsf(fv_text_sp(&scale,12)-36.0f)<0.001f);
    assert(fabsf(fv_text_sp(&scale,11.5f)-34.5f)<0.001f);
    assert(fv_text_sp(&scale,NAN)==0 && fv_text_sp(&scale,-1)==0);
    const float width=fvg_text_width(FVG_FONT_SANS,20,"AV kerning",false);
    assert(width>0 && fabsf(fvg_text_width(FVG_FONT_SANS,40,"AV kerning",false)-2*width)<0.01f);
    char out[128];
    fvg_text_elide(FVG_FONT_SANS,20,"A long filename with µT and a suffix",80,false,out,sizeof(out));
    assert(strstr(out,"…") && fvg_text_width(FVG_FONT_SANS,20,out,false)<=80);
    fvg_text_elide_filename(FVG_FONT_SANS,20,"hit-1791874141225-0-preroll.wav",180,out,sizeof(out));
    assert(strstr(out,"…") && strstr(out,"-preroll.wav") && fvg_text_width(FVG_FONT_SANS,20,out,false)<=180);
    char small[7]; fvg_text_elide(FVG_FONT_SANS,20,"µµµµµµ",1000,false,small,sizeof(small));
    assert(strcmp(small,"µ…")==0);
    fvg_text_elide(FVG_FONT_SANS,20,"Too narrow",1,false,out,sizeof(out)); assert(!out[0]);
    /* Fill and replace this bounded cache; previously a full neighbourhood
     * silently removed letters from subsequent labels. */
    for (int size=8;size<88;size++) for (int cp=33;cp<127;cp++) {
        char letter[2]={(char)cp,0}; assert(fvg_text_width(FVG_FONT_SANS,(float)size,letter,false)>0);
    }
    assert(fabsf(fvg_text_width(FVG_FONT_SANS,20,"AV kerning",false)-width)<0.01f);
    uint32_t *pixels=calloc(256u*128u,sizeof(*pixels)); assert(pixels);
    FvCanvas canvas; fvg_canvas_init(&canvas,pixels,256,128,256);
    fvg_text_center(&canvas,FVG_FONT_SANS,20,0,0,256,128,"Readable text",0xffffffffu,false);
    bool drawn=false; for (size_t i=0;i<256u*128u;i++) drawn|=pixels[i]!=0; assert(drawn);
    free(pixels); fvg_text_shutdown();
    assert(!fvg_font_ready(FVG_FONT_SANS));

    /* Cover-mode video blit fills every pixel in portrait and landscape with exact native color channels. */
    assert_cover_split(18, 32, 27, 60);
    assert_cover_split(32, 18, 60, 27);

    puts("Typography: em scaling, kerning, UTF-8 elision, cache replacement, rendering and cleanup passed");
    return 0;
}
