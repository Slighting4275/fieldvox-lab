/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "../../android/app/src/main/cpp/fv_camera_effects.h"
#include "../../android/app/src/main/cpp/fv_recording_data.h"
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
    puts("Camera: six RGB filters, torch scaling, YUV chroma, rotation/bounds, wraparound and swipe admission passed");
    return 0;
}
