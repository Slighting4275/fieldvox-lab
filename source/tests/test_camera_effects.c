/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "../../android/app/src/main/cpp/fv_camera_effects.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t filtered(uint32_t source, FvCameraFilter filter, bool torch) {
    uint32_t out=0;
    const bool ok=fv_camera_filter_frame(&source,1,1,&out,1,0,filter,torch,NULL,NULL);
    assert(ok);
    return out;
}
static int swipe(float distance, float vertical, int64_t duration, bool begin_ok, bool end_ok) {
    FvCameraGesture g={0};
    const int64_t start=1000000000;
    fv_camera_gesture_begin(&g,600,900,start,begin_ok);
    for (int i=1; i<=20; i++) fv_camera_gesture_move(&g,600+distance*(float)i/20.0f,start+duration*i/20);
    const int result=fv_camera_gesture_end(&g,600+distance,900+vertical,start+duration,end_ok);
    assert(!g.active && !g.count);
    return result;
}
int main(void) {
    assert(filtered(0xff123456u,FV_CAM_FILTER_NORMAL,false)==0xff123456u);
    assert(filtered(0xffff0000u,FV_CAM_FILTER_GREEN,false)==0xff335933u);
    assert(filtered(0xffff0000u,FV_CAM_FILTER_PURPLE,false)==0xff593359u);
    assert(filtered(0xffff0000u,FV_CAM_FILTER_GREEN,true)==0xffccffccu);
    assert(filtered(0xffff0000u,FV_CAM_FILTER_INVERTED,false)==0xff00ffffu);
    assert(filtered(0xff000000u,FV_CAM_FILTER_HEAT,false)==0xff0000ffu);
    assert(filtered(0xffffffffu,FV_CAM_FILTER_HEAT,false)==0xffff0000u);
    assert(filtered(0xffff0000u,FV_CAM_FILTER_HEAT,false)==0xffaaaa55u);
    assert(filtered(0xff000000u,FV_CAM_FILTER_DEPTH,false)==0xff000000u);
    assert(filtered(0xff808080u,FV_CAM_FILTER_DEPTH,false)==0xff7a7a7au);
    assert(filtered(0xffffffffu,FV_CAM_FILTER_DEPTH,false)==0xffccccccu);
    assert(filtered(0xffffffffu,FV_CAM_FILTER_OFF,false)==0xff000000u);
    FvCameraFilter filter=FV_CAM_FILTER_NORMAL;
    for (int i=0; i<FV_CAM_FILTER_COUNT; i++) filter=fv_camera_filter_step(filter,1);
    assert(filter==FV_CAM_FILTER_NORMAL);
    assert(fv_camera_filter_step(filter,-1)==FV_CAM_FILTER_INVERTED);
    assert(fv_camera_filter_step(FV_CAM_FILTER_INVERTED,1)==FV_CAM_FILTER_NORMAL);
    assert(fv_camera_filter_step(FV_CAM_FILTER_OFF,1)==FV_CAM_FILTER_NORMAL);
    assert(strcmp(fv_camera_filter_name(FV_CAM_FILTER_PURPLE),"PURPLE")==0);

    /* Chroma reaches the normal/inverted filter instead of becoming grayscale. */
    const uint8_t y[4]={81,81,81,81}, u[1]={90}, v[1]={240};
    const uint8_t *planes[3]={y,u,v};
    const size_t lengths[3]={4,1,1};
    const int rows[3]={2,1,1}, pixels[3]={1,1,1};
    uint32_t source[4], output[4];
    const bool converted=fv_yuv420_to_argb(source,2,2,2,2,0,0,2,2,planes,lengths,rows,pixels);
    assert(converted && ((source[0]>>16u)&255u)>240u && (source[0]&255u)<20u);
    const bool normal=fv_camera_filter_frame(source,2,2,output,4,0,FV_CAM_FILTER_NORMAL,false,NULL,NULL);
    assert(normal && memcmp(source,output,sizeof(source))==0);
    const uint32_t corners[6]={0xff010101u,0xff020202u,0xff030303u,0xff040404u,0xff050505u,0xff060606u};
    const unsigned expected[4][6]={{0,1,2,3,4,5},{3,0,4,1,5,2},{5,4,3,2,1,0},{2,5,1,4,0,3}};
    for (int angle=0; angle<4; angle++) for (int f=0; f<FV_CAM_FILTER_COUNT; f++) {
        uint32_t dst[8]={0xdeadbeefu,0,0,0,0,0,0,0xdeadbeefu};
        int w=0,h=0;
        const bool ok=fv_camera_filter_frame(corners,3,2,dst+1,6,90*angle,(FvCameraFilter)f,false,&w,&h);
        assert(ok && w==(angle%2 ? 2 : 3) && h==(angle%2 ? 3 : 2));
        assert(dst[0]==0xdeadbeefu && dst[7]==0xdeadbeefu);
        if (f==FV_CAM_FILTER_NORMAL) for (int i=0; i<6; i++) assert(dst[i+1]==corners[expected[angle][i]]);
    }
    uint32_t untouched=0xabcdef01u; int w=-1,h=-1;
    bool ok=fv_camera_filter_frame(corners,3,2,&untouched,1,0,FV_CAM_FILTER_NORMAL,false,&w,&h);
    assert(!ok && untouched==0xabcdef01u && w==-1 && h==-1);
    ok=fv_camera_filter_frame(corners,1,1,&untouched,1,45,FV_CAM_FILTER_NORMAL,false,NULL,NULL); assert(!ok);
    ok=fv_camera_filter_frame(corners,1,1,&untouched,1,0,(FvCameraFilter)-1,false,NULL,NULL); assert(!ok);

    assert(swipe(-400,0,200000000,true,true)==1);
    assert(swipe(400,20,200000000,true,true)==-1);
    assert(swipe(-100,0,200000000,true,true)==0); /* Strict distance threshold. */
    assert(swipe(-99,0,200000000,true,true)==0);
    assert(swipe(-200,0,2000000000,true,true)==0); /* Exactly 100 px/s. */
    assert(swipe(-200,0,3000000000,true,true)==0);
    assert(swipe(-200,201,200000000,true,true)==0);
    assert(swipe(-200,200,200000000,true,true)==0);
    assert(swipe(-400,0,200000000,false,true)==0);
    assert(swipe(-400,0,200000000,true,false)==0);
    FvCameraGesture g;
    fv_camera_gesture_begin(&g,600,900,0,true);
    fv_camera_gesture_move(&g,200,100000000);
    int result=fv_camera_gesture_end(&g,200,900,2100000000,true); assert(result==0); /* Held after movement. */
    fv_camera_gesture_begin(&g,600,900,0,true);
    fv_camera_gesture_cancel(&g);
    result=fv_camera_gesture_end(&g,200,900,100000000,true); assert(result==0);
    fv_camera_gesture_begin(&g,600,900,100000000,true);
    fv_camera_gesture_move(&g,200,0); assert(!g.active);
    fv_camera_gesture_begin(&g,NAN,900,0,true); assert(!g.active);
    assert(fv_camera_compensation_degrees(90, 0) == 90);
    assert(fv_camera_compensation_degrees(90, 90) == 0);
    assert(fv_camera_compensation_degrees(90, 270) == 180);

    {
        uint8_t iy[] = {235, 235, 235, 235}, iuv[] = {128, 128};
        const uint8_t *iplanes[] = {iy, iuv, iuv + 1};
        const size_t ilengths[] = {4, 2, 1};
        int irow[] = {2, 2, 2}, ipixel[] = {1, 2, 2};
        uint32_t image[5] = {0, 0, 0, 0, 0x12345678};
        assert(fv_yuv420_to_argb(image, 2, 2, 2, 2, 0, 0, 2, 2, iplanes, ilengths, irow, ipixel));
        for (int i = 0; i < 4; i++) assert(image[i] == 0xffffffffu);
        assert(image[4] == 0x12345678);
        memset(iy, 16, sizeof(iy));
        assert(fv_yuv420_to_argb(image, 2, 2, 2, 2, 0, 0, 2, 2, iplanes, ilengths, irow, ipixel));
        assert(image[0] == 0xff000000u);
        memset(iy, 63, sizeof(iy)); iuv[0] = 102; iuv[1] = 240;
        assert(fv_yuv420_to_argb(image, 2, 2, 2, 2, 0, 0, 2, 2, iplanes, ilengths, irow, ipixel));
        assert(((image[0] >> 16) & 255u) >= 254u && ((image[0] >> 8) & 255u) <= 1u && (image[0] & 255u) <= 1u);
        const size_t bad_len[] = {4, 2, 0};
        assert(!fv_yuv420_to_argb(image, 2, 2, 2, 2, 0, 0, 2, 2, iplanes, bad_len, irow, ipixel));
        irow[0] = 0; assert(!fv_yuv420_to_argb(image, 2, 2, 2, 2, 0, 0, 2, 2, iplanes, ilengths, irow, ipixel));
        irow[0] = 2; assert(!fv_yuv420_to_argb(image, 2, 2, 2, 2, -1, 0, 2, 2, iplanes, ilengths, irow, ipixel));
    }

    /* Front-facing orientation compensation and horizontal mirror */
    assert(fv_camera_compensation_degrees_facing(270, 0, true) == 270);
    assert(fv_camera_compensation_degrees_facing(270, 90, true) == 0);
    assert(fv_camera_compensation_degrees_facing(270, 270, true) == 180);
    {
        const uint32_t lr[2] = {0xFF112233u, 0xFF445566u};
        uint32_t out_m[2] = {0};
        assert(fv_camera_filter_frame_ex(lr, 2, 1, out_m, 2, 0, true, FV_CAM_FILTER_NORMAL, false, NULL, NULL));
        assert(out_m[0] == 0xFF445566u && out_m[1] == 0xFF112233u);
    }

    /* Multi-lens discovery ordering and human-readable labels */
    FvCameraLensInfo lenses[4] = {
        {.id = "1", .focal_mm = 2.5f, .front_facing = true},
        {.id = "2", .focal_mm = 1.9f, .front_facing = false},
        {.id = "0", .focal_mm = 6.9f, .front_facing = false},
        {.id = "3", .focal_mm = 18.0f, .front_facing = false},
    };
    fv_camera_build_lens_labels(lenses, 4u);
    assert(strcmp(lenses[0].id, "0") == 0 && strcmp(lenses[0].label, "WIDE") == 0);
    assert(strcmp(lenses[1].id, "2") == 0 && strcmp(lenses[1].label, "ULTRA") == 0);
    assert(strcmp(lenses[2].id, "3") == 0 && strcmp(lenses[2].label, "TELE") == 0);
    assert(strcmp(lenses[3].id, "1") == 0 && strcmp(lenses[3].label, "FRONT") == 0);

    FvCameraLensInfo logical[5] = {
        {.id = "3", .parent_id = "0", .focal_mm = 2.0f},
        {.id = "2", .parent_id = "0", .focal_mm = 6.9f},
        {.id = "4", .parent_id = "0", .focal_mm = 17.9f},
        {.id = "1", .focal_mm = 2.7f, .front_facing = true},
        {.id = "0", .focal_mm = 6.9f},
    };
    fv_camera_build_lens_labels(logical, 5u);
    assert(strcmp(logical[0].label, "BACK") == 0 && !logical[0].parent_id[0]);
    assert(strcmp(logical[1].label, "ULTRA") == 0);
    assert(strcmp(logical[2].label, "WIDE") == 0);
    assert(strcmp(logical[3].label, "TELE") == 0);
    assert(strcmp(logical[4].label, "FRONT") == 0);

    puts("Camera: six RGB filters, torch scaling, YUV chroma, rotation/bounds, wraparound and swipe admission passed");
    return 0;
}
