/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_camera_effects.h"
#include <math.h>
#include <string.h>

const char *fv_camera_filter_name(FvCameraFilter filter) {
    static const char *const names[] = {"NORMAL", "GREEN", "PURPLE", "DEPTH", "HEAT", "INVERTED", "STEALTH"};
    return filter >= FV_CAM_FILTER_NORMAL && filter <= FV_CAM_FILTER_OFF ? names[filter] : "OFF";
}
FvCameraFilter fv_camera_filter_step(FvCameraFilter filter, int direction) {
    if (filter < FV_CAM_FILTER_NORMAL || filter >= FV_CAM_FILTER_COUNT) return FV_CAM_FILTER_NORMAL;
    if (!direction) return filter;
    return (FvCameraFilter)(((int)filter + (direction > 0 ? 1 : FV_CAM_FILTER_COUNT-1)) % FV_CAM_FILTER_COUNT);
}
static uint32_t channel(float value) {
    if (value <= 0) return 0;
    if (value >= 255) return 255;
    return (uint32_t)(value + 0.5f);
}
static uint32_t rgb(float r, float g, float b) {
    return 0xff000000u | (channel(r) << 16u) | (channel(g) << 8u) | channel(b);
}
static float mean_rgb(uint32_t color) {
    return (float)(((color >> 16u) & 255u) + ((color >> 8u) & 255u) + (color & 255u)) / (3.0f * 255.0f);
}
/* Bilinear sampling with clamped edges mirrors the external texture sampler. */
static float sample_mean(const uint32_t *src, int w, int h, float x, float y) {
    x = fminf(fmaxf(x, 0), (float)(w-1));
    y = fminf(fmaxf(y, 0), (float)(h-1));
    const int x0=(int)x, y0=(int)y, x1=x0+1<w ? x0+1 : x0, y1=y0+1<h ? y0+1 : y0;
    const float tx=x-(float)x0, ty=y-(float)y0;
    const float a=mean_rgb(src[(size_t)y0*(size_t)w+(size_t)x0]);
    const float b=mean_rgb(src[(size_t)y0*(size_t)w+(size_t)x1]);
    const float c=mean_rgb(src[(size_t)y1*(size_t)w+(size_t)x0]);
    const float d=mean_rgb(src[(size_t)y1*(size_t)w+(size_t)x1]);
    return (a+(b-a)*tx)*(1-ty)+(c+(d-c)*tx)*ty;
}
static uint32_t depth_color(const uint32_t *src, int w, int h, int x, int y) {
    float average=0;
    for (int dy=-5; dy<=5; dy+=5) for (int dx=-5; dx<=5; dx+=5)
        average+=sample_mean(src,w,h,(float)x+(float)dx*(float)w/720.0f,(float)y+(float)dy*(float)h/1280.0f);
    /* The declared shader divides the nine samples by 10 squared, not by nine. */
    average/=100.0f;
    float level;
    if (average<0.01f) level=0;
    else if (average<0.02f) level=0.1f;
    else if (average<0.04f) level=0.2f;
    else if (average<0.06f) level=0.3f;
    else if (average<0.08f) level=0.4f;
    else if (average<0.10f) level=0.5f;
    else if (average<0.12f) level=0.6f;
    else if (average<0.14f) level=0.7f;
    else if (average<0.16f) level=0.8f;
    else if (average<0.18f) level=0.9f;
    else level=1;
    return rgb(level*1.6f*255.0f,level*1.6f*255.0f,level*1.6f*255.0f);
}
bool fv_camera_filter_frame(const uint32_t *src, int w, int h, uint32_t *dst, size_t capacity,
                            int rotation, FvCameraFilter filter, bool torch, int *out_w, int *out_h) {
    if (!src || !dst || src==dst || w<=0 || h<=0 || w>4096 || h>4096 ||
        capacity<(size_t)w*(size_t)h || filter<FV_CAM_FILTER_NORMAL || filter>FV_CAM_FILTER_OFF) return false;
    rotation=(rotation%360+360)%360;
    if (rotation%90) return false;
    const int dw=(rotation==90 || rotation==270) ? h : w;
    const int dh=(rotation==90 || rotation==270) ? w : h;
    for (int y=0; y<h; y++) for (int x=0; x<w; x++) {
        const uint32_t color=src[(size_t)y*(size_t)w+(size_t)x];
        const float r=(float)((color>>16u)&255u), g=(float)((color>>8u)&255u), b=(float)(color&255u);
        uint32_t out;
        switch (filter) {
            case FV_CAM_FILTER_NORMAL: out=color|0xff000000u; break;
            case FV_CAM_FILTER_GREEN:
            case FV_CAM_FILTER_PURPLE: {
                const float boost=torch ? 4.0f : 1.0f;
                const float low=0.2f*(r+g+b)*boost, high=(0.349f*r+0.686f*g+0.168f*b)*boost;
                out=filter==FV_CAM_FILTER_GREEN ? rgb(low,high,low) : rgb(high,low,high);
                break;
            }
            case FV_CAM_FILTER_DEPTH: out=depth_color(src,w,h,x,y); break;
            case FV_CAM_FILTER_HEAT: {
                const float lum=(r+g+b)/(3.0f*255.0f);
                out=lum<0.5f ? rgb(lum*510.0f,lum*510.0f,(1.0f-lum*2.0f)*255.0f) :
                    rgb(255.0f,(2.0f-lum*2.0f)*255.0f,0);
                break;
            }
            case FV_CAM_FILTER_INVERTED: out=rgb(255-r,255-g,255-b); break;
            case FV_CAM_FILTER_OFF: default: out=0xff000000u; break;
        }
        int dx=x, dy=y;
        if (rotation==90) { dx=h-1-y; dy=x; }
        else if (rotation==180) { dx=w-1-x; dy=h-1-y; }
        else if (rotation==270) { dx=y; dy=w-1-x; }
        dst[(size_t)dy*(size_t)dw+(size_t)dx]=out;
    }
    if (out_w) *out_w=dw;
    if (out_h) *out_h=dh;
    return true;
}
void fv_camera_gesture_cancel(FvCameraGesture *gesture) {
    if (gesture) *gesture=(FvCameraGesture){0};
}
void fv_camera_gesture_begin(FvCameraGesture *g, float x, float y, int64_t ns, bool eligible) {
    if (!g) return;
    *g=(FvCameraGesture){0};
    if (!eligible || !isfinite(x) || !isfinite(y) || ns<0) return;
    g->active=true; g->start_x=x; g->start_y=y;
    fv_camera_gesture_move(g,x,ns);
}
void fv_camera_gesture_move(FvCameraGesture *g, float x, int64_t ns) {
    if (!g || !g->active) return;
    if (!isfinite(x) || ns<0 || (g->count && ns<g->samples[g->count-1u].ns)) {
        fv_camera_gesture_cancel(g); return;
    }
    if (g->count && ns==g->samples[g->count-1u].ns) {
        g->samples[g->count-1u].x=x; return;
    }
    if (g->count==FV_CAMERA_GESTURE_SAMPLES) {
        memmove(g->samples,g->samples+1,(FV_CAMERA_GESTURE_SAMPLES-1u)*sizeof(g->samples[0]));
        g->count--;
    }
    g->samples[g->count++]=(FvCameraGestureSample){x,ns};
}
int fv_camera_gesture_end(FvCameraGesture *g, float x, float y, int64_t ns, bool eligible) {
    if (!g || !g->active) return 0;
    fv_camera_gesture_move(g,x,ns);
    int direction=0;
    const float dx=x-g->start_x, dy=y-g->start_y;
    if (g->active && eligible && isfinite(y) && fabsf(dx)>100.0f && fabsf(dx)>fabsf(dy)) {
        /* Least-squares velocity over recent event timestamps, in physical px/s. */
        double st=0, sx=0, stt=0, stx=0, n=0;
        for (unsigned i=0; i<g->count; i++) {
            const int64_t age=ns-g->samples[i].ns;
            if (age>200000000) continue;
            const double t=-(double)age/1e9, xx=g->samples[i].x;
            st+=t; sx+=xx; stt+=t*t; stx+=t*xx; n+=1;
        }
        const double denominator=n*stt-st*st;
        if (n>=2 && denominator>0) {
            const double velocity=(n*stx-st*sx)/denominator;
            if (fabs(velocity)>100) direction=dx<0 ? 1 : -1;
        }
    }
    fv_camera_gesture_cancel(g);
    return direction;
}
static uint8_t clamp_u8(int value) { return (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value)); }
bool fv_yuv420_to_argb(uint32_t *dst, int dw, int dh, int sw, int sh, int left, int top, int right, int bottom,
                       const uint8_t *planes[3], const size_t lengths[3], const int rs[3], const int ps[3]) {
    if (!dst || !planes || !lengths || !rs || !ps || dw <= 0 || dh <= 0 || dw > 4096 || dh > 4096 ||
        sw <= 0 || sh <= 0 || sw > 8192 || sh > 8192 || left < 0 || top < 0 || right > sw || bottom > sh ||
        right <= left || bottom <= top) return false;
    for (int p = 0; p < 3; p++) {
        if (!planes[p] || rs[p] <= 0 || ps[p] <= 0 || rs[p] > 1048576 || ps[p] > 16) return false;
        const size_t xx = (size_t)(right - 1) / (p ? 2u : 1u), yy = (size_t)(bottom - 1) / (p ? 2u : 1u);
        if (xx * (size_t)ps[p] >= (size_t)rs[p] || yy * (size_t)rs[p] + xx * (size_t)ps[p] >= lengths[p]) return false;
    }
    for (int y = 0; y < dh; y++) {
        const int sy = top + (int)((int64_t)y * (bottom - top) / dh);
        for (int x = 0; x < dw; x++) {
            const int sx = left + (int)((int64_t)x * (right - left) / dw);
            const int l = (int)planes[0][(size_t)sy * (size_t)rs[0] + (size_t)sx * (size_t)ps[0]] - 16;
            const int u = (int)planes[1][(size_t)(sy / 2) * (size_t)rs[1] + (size_t)(sx / 2) * (size_t)ps[1]] - 128;
            const int v = (int)planes[2][(size_t)(sy / 2) * (size_t)rs[2] + (size_t)(sx / 2) * (size_t)ps[2]] - 128;
            const uint32_t r = clamp_u8((298 * l + 459 * v + 128) >> 8);
            const uint32_t g = clamp_u8((298 * l - 55 * u - 136 * v + 128) >> 8);
            const uint32_t b = clamp_u8((298 * l + 541 * u + 128) >> 8);
            dst[(size_t)y * (size_t)dw + (size_t)x] = 0xff000000u | (r << 16u) | (g << 8u) | b;
        }
    }
    return true;
}
int fv_camera_compensation_degrees(int sensor_orientation_deg, int display_rotation_deg) {
    const int s = ((sensor_orientation_deg % 360) + 360) % 360;
    const int d = ((display_rotation_deg % 360) + 360) % 360;
    return (s - d + 360) % 360;
}
