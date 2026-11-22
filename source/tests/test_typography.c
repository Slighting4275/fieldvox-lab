/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "../../android/app/src/main/cpp/fv_gfx.h"
#include "../../android/app/src/main/cpp/fv_android_text.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    puts("Typography: em scaling, kerning, UTF-8 elision, cache replacement, rendering and cleanup passed");
    return 0;
}
