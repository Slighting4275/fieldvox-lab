/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_android_text.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
static const float sizes[FV_TEXT_SIZE_COUNT]={8,11,12,14,16,20};
float fv_text_sp(const FvTextScale *scale,float sp) {
    if (!scale || !isfinite(sp) || sp<=0) return 0;
    if (sp<=sizes[0]) return scale->px[0]*sp/sizes[0];
    for (int i=1;i<FV_TEXT_SIZE_COUNT;i++) if (sp<=sizes[i]) {
        const float t=(sp-sizes[i-1])/(sizes[i]-sizes[i-1]);
        return scale->px[i-1]+t*(scale->px[i]-scale->px[i-1]);
    }
    return scale->px[FV_TEXT_SIZE_COUNT-1]*sp/sizes[FV_TEXT_SIZE_COUNT-1];
}
#ifdef __ANDROID__
#include <jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include "fv_gfx.h"
#include "fv_jni_helpers.h"
static JavaVM *java_vm;
static _Thread_local struct {
    JNIEnv *env;
    bool initialized,failed,attached;
    jclass paint_class,bitmap_class,canvas_class,rect_class;
    jobject config,paint[FVG_FONT_COUNT];
    jmethodID size,measure,measure_range,bounds,metrics,bitmap_new,canvas_new,draw,rect_new;
    jfieldID left,top,right,bottom,metric_top,metric_bottom;
    float last_size[FVG_FONT_COUNT],top_px[FVG_FONT_COUNT],bottom_px[FVG_FONT_COUNT];
} text;
static bool prepare(void) {
    if (text.initialized) return true;
    if (text.failed || !java_vm) return false;
    text.failed=true;
    JNIEnv *env=NULL;
    if ((*java_vm)->GetEnv(java_vm,(void **)&env,JNI_VERSION_1_6)!=JNI_OK) {
        if ((*java_vm)->AttachCurrentThread(java_vm,&env,NULL)!=JNI_OK) return false;
        text.attached=true;
    }
    text.env=env;
    if ((*env)->PushLocalFrame(env,32)!=JNI_OK) { fv_jni_check(env,"FieldVoxText","font initialization frame",false); return false; }
    bool ok=false;
    text.paint_class=fv_jni_global_class(env,"FieldVoxText","android/graphics/Paint",false);
    text.bitmap_class=fv_jni_global_class(env,"FieldVoxText","android/graphics/Bitmap",false);
    text.canvas_class=fv_jni_global_class(env,"FieldVoxText","android/graphics/Canvas",false);
    text.rect_class=fv_jni_global_class(env,"FieldVoxText","android/graphics/Rect",false);
    jclass typeface=fv_jni_find_class(env,"FieldVoxText","android/graphics/Typeface",false);
    jclass config=fv_jni_find_class(env,"FieldVoxText","android/graphics/Bitmap$Config",false);
    jclass fm=fv_jni_find_class(env,"FieldVoxText","android/graphics/Paint$FontMetrics",false);
    if (!typeface || !config || !fm || !text.paint_class || !text.bitmap_class || !text.canvas_class || !text.rect_class) goto done;
#define M(member,cls,name,sig) text.member=fv_jni_method(env,"FieldVoxText",cls,name,sig,false); if (!text.member) goto done
    M(size,text.paint_class,"setTextSize","(F)V");
    M(measure,text.paint_class,"measureText","(Ljava/lang/String;)F");
    M(measure_range,text.paint_class,"measureText","(Ljava/lang/String;II)F");
    M(bounds,text.paint_class,"getTextBounds","(Ljava/lang/String;IILandroid/graphics/Rect;)V");
    M(metrics,text.paint_class,"getFontMetrics","()Landroid/graphics/Paint$FontMetrics;");
    M(canvas_new,text.canvas_class,"<init>","(Landroid/graphics/Bitmap;)V");
    M(draw,text.canvas_class,"drawText","(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
    M(rect_new,text.rect_class,"<init>","()V");
#undef M
    text.left=fv_jni_field(env,"FieldVoxText",text.rect_class,"left","I",false);
    text.top=fv_jni_field(env,"FieldVoxText",text.rect_class,"top","I",false);
    text.right=fv_jni_field(env,"FieldVoxText",text.rect_class,"right","I",false);
    text.bottom=fv_jni_field(env,"FieldVoxText",text.rect_class,"bottom","I",false);
    text.metric_top=fv_jni_field(env,"FieldVoxText",fm,"top","F",false);
    text.metric_bottom=fv_jni_field(env,"FieldVoxText",fm,"bottom","F",false);
    if (!text.left || !text.top || !text.right || !text.bottom || !text.metric_top || !text.metric_bottom) goto done;
    text.bitmap_new=fv_jni_static_method(env,"FieldVoxText",text.bitmap_class,"createBitmap","(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;",false);
    if (!text.bitmap_new) goto done;
    const jfieldID argb=fv_jni_static_field(env,"FieldVoxText",config,"ARGB_8888","Landroid/graphics/Bitmap$Config;",false);
    if (!argb) goto done;
    jobject cfg=(*env)->GetStaticObjectField(env,config,argb);
    if (!fv_jni_check(env,"FieldVoxText","ARGB8888",false) || !cfg) goto done;
    text.config=(*env)->NewGlobalRef(env,cfg);
    if (!fv_jni_check(env,"FieldVoxText","bitmap config reference",false) || !text.config) goto done;
    jmethodID constructor=fv_jni_method(env,"FieldVoxText",text.paint_class,"<init>","(I)V",false);
    jmethodID face=fv_jni_method(env,"FieldVoxText",text.paint_class,"setTypeface","(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;",false);
    jmethodID color=fv_jni_method(env,"FieldVoxText",text.paint_class,"setColor","(I)V",false);
    jmethodID create=fv_jni_static_method(env,"FieldVoxText",typeface,"create","(Ljava/lang/String;I)Landroid/graphics/Typeface;",false);
    if (!constructor || !face || !color || !create) goto done;
    for (int i=0;i<FVG_FONT_COUNT;i++) {
        jstring family=(*env)->NewStringUTF(env,i==FVG_FONT_MONO ? "monospace" : (i==FVG_FONT_MEDIUM ? "sans-serif-medium" : "sans-serif"));
        if (!fv_jni_check(env,"FieldVoxText","font family",false) || !family) goto done;
        jobject font=(*env)->CallStaticObjectMethod(env,typeface,create,family,i==FVG_FONT_BOLD ? 1 : 0);
        if (!fv_jni_check(env,"FieldVoxText","font face",false) || !font) goto done;
        jobject paint=(*env)->NewObject(env,text.paint_class,constructor,129); /* anti-alias + subpixel text */
        if (!fv_jni_check(env,"FieldVoxText","Paint constructor",false) || !paint) goto done;
        (*env)->CallObjectMethod(env,paint,face,font);
        if (!fv_jni_check(env,"FieldVoxText","font assignment",false)) goto done;
        (*env)->CallVoidMethod(env,paint,color,(jint)-1);
        if (!fv_jni_check(env,"FieldVoxText","glyph color",false)) goto done;
        text.paint[i]=(*env)->NewGlobalRef(env,paint);
        if (!fv_jni_check(env,"FieldVoxText","paint reference",false) || !text.paint[i]) goto done;
        (*env)->DeleteLocalRef(env,family); (*env)->DeleteLocalRef(env,font); (*env)->DeleteLocalRef(env,paint);
    }
    ok=true;
    __android_log_print(ANDROID_LOG_INFO,"FieldVoxText","C23 platform fonts: sans-serif regular, medium and bold; Android em metrics");
done:
    (*env)->PopLocalFrame(env,NULL); text.initialized=ok; text.failed=!ok; return ok;
}
static bool set_size(int font,float pixels) {
    if (font<0 || font>=FVG_FONT_COUNT || !isfinite(pixels) || pixels<=0 || pixels>512 || !prepare()) return false;
    JNIEnv *env=text.env;
    if (pixels==text.last_size[font]) return true;
    (*env)->CallVoidMethod(env,text.paint[font],text.size,pixels);
    if (!fv_jni_check(env,"FieldVoxText","Paint size",false)) return false;
    jobject fm=(*env)->CallObjectMethod(env,text.paint[font],text.metrics);
    if (!fv_jni_check(env,"FieldVoxText","font metrics",false) || !fm) return false;
    text.top_px[font]=(*env)->GetFloatField(env,fm,text.metric_top);
    text.bottom_px[font]=(*env)->GetFloatField(env,fm,text.metric_bottom);
    (*env)->DeleteLocalRef(env,fm); text.last_size[font]=pixels; return true;
}
static int utf16(uint32_t cp,jchar *chars) {
    if (cp>0x10ffffu || (cp>=0xd800u && cp<=0xdfffu)) cp='?';
    if (cp<=0xffffu) { chars[0]=(jchar)cp; return 1; }
    cp-=0x10000u; chars[0]=(jchar)(0xd800u+(cp>>10u)); chars[1]=(jchar)(0xdc00u+(cp&1023u)); return 2;
}
void fv_android_text_set_vm(void *vm) { java_vm=vm; }
bool fv_android_text_available(void) { return java_vm!=NULL; }
FvTextScale fv_android_text_scale(void *activity,float density) {
    FvTextScale scale; for (int i=0;i<FV_TEXT_SIZE_COUNT;i++) scale.px[i]=sizes[i]*density;
    if (!java_vm || !activity) return scale;
    JNIEnv *env=NULL; if ((*java_vm)->GetEnv(java_vm,(void **)&env,JNI_VERSION_1_6)!=JNI_OK) return scale;
    if ((*env)->PushLocalFrame(env,16)!=JNI_OK) { fv_jni_check(env,"FieldVoxText","sp frame",false); return scale; }
    jclass context=fv_jni_find_class(env,"FieldVoxText","android/content/Context",false);
    jclass resources=fv_jni_find_class(env,"FieldVoxText","android/content/res/Resources",false);
    jclass value=fv_jni_find_class(env,"FieldVoxText","android/util/TypedValue",false);
    if (!context || !resources || !value) goto done;
    jmethodID get_resources=fv_jni_method(env,"FieldVoxText",context,"getResources","()Landroid/content/res/Resources;",false);
    jmethodID get_metrics=fv_jni_method(env,"FieldVoxText",resources,"getDisplayMetrics","()Landroid/util/DisplayMetrics;",false);
    jmethodID dimension=fv_jni_static_method(env,"FieldVoxText",value,"applyDimension","(IFLandroid/util/DisplayMetrics;)F",false);
    if (!get_resources || !get_metrics || !dimension) goto done;
    jobject res=(*env)->CallObjectMethod(env,activity,get_resources);
    if (!fv_jni_check(env,"FieldVoxText","getResources",false) || !res) goto done;
    jobject metrics=(*env)->CallObjectMethod(env,res,get_metrics);
    if (!fv_jni_check(env,"FieldVoxText","getDisplayMetrics",false) || !metrics) goto done;
    for (int i=0;i<FV_TEXT_SIZE_COUNT;i++) {
        const float px=(*env)->CallStaticFloatMethod(env,value,dimension,2,sizes[i],metrics);
        if (!fv_jni_check(env,"FieldVoxText","sp conversion",false)) break;
        if (isfinite(px) && px>0 && px<=512) scale.px[i]=px;
    }
    __android_log_print(ANDROID_LOG_INFO,"FieldVoxText","sp sizes: 8=%.2fpx 11=%.2fpx 12=%.2fpx 14=%.2fpx 16=%.2fpx",(double)scale.px[0],(double)scale.px[1],(double)scale.px[2],(double)scale.px[3],(double)scale.px[4]);
done:
    (*env)->PopLocalFrame(env,NULL); return scale;
}
bool fv_android_text_metrics(int font,float pixels,float *top,float *bottom) {
    if (!top || !bottom || !set_size(font,pixels)) return false;
    *top=text.top_px[font]; *bottom=text.bottom_px[font]; return true;
}
bool fv_android_text_glyph(int font,float pixels,uint32_t cp,FvTextGlyph *out) {
    if (!out || !set_size(font,pixels)) return false;
    JNIEnv *env=text.env;
    if ((*env)->PushLocalFrame(env,16)!=JNI_OK) { fv_jni_check(env,"FieldVoxText","glyph frame",false); return false; }
    bool ok=false; *out=(FvTextGlyph){0}; jchar chars[2]; const int length=utf16(cp,chars);
    jstring string=(*env)->NewString(env,chars,length);
    if (!fv_jni_check(env,"FieldVoxText","glyph string",false) || !string) goto done;
    out->advance=(*env)->CallFloatMethod(env,text.paint[font],text.measure,string);
    if (!fv_jni_check(env,"FieldVoxText","glyph advance",false)) goto done;
    jobject rect=(*env)->NewObject(env,text.rect_class,text.rect_new);
    if (!fv_jni_check(env,"FieldVoxText","glyph rect",false) || !rect) goto done;
    (*env)->CallVoidMethod(env,text.paint[font],text.bounds,string,0,length,rect);
    if (!fv_jni_check(env,"FieldVoxText","glyph bounds",false)) goto done;
    const int left=(*env)->GetIntField(env,rect,text.left),top=(*env)->GetIntField(env,rect,text.top);
    const int right=(*env)->GetIntField(env,rect,text.right),bottom=(*env)->GetIntField(env,rect,text.bottom);
    if (right<=left || bottom<=top) { ok=true; goto done; }
    if (right-left>1024 || bottom-top>1024) goto done;
    out->w=right-left+4; out->h=bottom-top+4; out->xoff=left-2; out->yoff=top-2;
    jobject bitmap=(*env)->CallStaticObjectMethod(env,text.bitmap_class,text.bitmap_new,out->w,out->h,text.config);
    if (!fv_jni_check(env,"FieldVoxText","glyph bitmap",false) || !bitmap) goto done;
    jobject canvas=(*env)->NewObject(env,text.canvas_class,text.canvas_new,bitmap);
    if (!fv_jni_check(env,"FieldVoxText","glyph canvas",false) || !canvas) goto done;
    (*env)->CallVoidMethod(env,canvas,text.draw,string,(float)-out->xoff,(float)-out->yoff,text.paint[font]);
    if (!fv_jni_check(env,"FieldVoxText","glyph draw",false)) goto done;
    AndroidBitmapInfo info; void *pixels_data=NULL;
    if (AndroidBitmap_getInfo(env,bitmap,&info)!=ANDROID_BITMAP_RESULT_SUCCESS || info.format!=ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        info.width!=(uint32_t)out->w || info.height!=(uint32_t)out->h || info.stride<info.width*4u) goto done;
    out->bitmap=malloc((size_t)out->w*(size_t)out->h); if (!out->bitmap) goto done;
    if (AndroidBitmap_lockPixels(env,bitmap,&pixels_data)!=ANDROID_BITMAP_RESULT_SUCCESS) goto done;
    for (int y=0;y<out->h;y++) for (int x=0;x<out->w;x++)
        out->bitmap[(size_t)y*(size_t)out->w+(size_t)x]=((const uint8_t *)pixels_data)[(size_t)y*info.stride+(size_t)x*4u+3u];
    ok=AndroidBitmap_unlockPixels(env,bitmap)==ANDROID_BITMAP_RESULT_SUCCESS;
done:
    if (!ok) { fv_jni_check(env,"FieldVoxText","glyph rasterization",false); free(out->bitmap); *out=(FvTextGlyph){0}; }
    (*env)->PopLocalFrame(env,NULL); return ok;
}
float fv_android_text_kerning(int font,float pixels,uint32_t left,uint32_t right) {
    if (!set_size(font,pixels)) return 0;
    JNIEnv *env=text.env; jchar chars[4]; int first=utf16(left,chars); const int count=first+utf16(right,chars+first);
    jstring pair=(*env)->NewString(env,chars,count);
    if (!fv_jni_check(env,"FieldVoxText","kerning string",false) || !pair) return 0;
    const float both=(*env)->CallFloatMethod(env,text.paint[font],text.measure,pair);
    if (!fv_jni_check(env,"FieldVoxText","pair advance",false)) { (*env)->DeleteLocalRef(env,pair); return 0; }
    const float a=(*env)->CallFloatMethod(env,text.paint[font],text.measure_range,pair,0,first);
    if (!fv_jni_check(env,"FieldVoxText","left advance",false)) { (*env)->DeleteLocalRef(env,pair); return 0; }
    const float b=(*env)->CallFloatMethod(env,text.paint[font],text.measure_range,pair,first,count);
    const bool ok=fv_jni_check(env,"FieldVoxText","right advance",false); (*env)->DeleteLocalRef(env,pair);
    return ok ? both-a-b : 0;
}
void fv_android_text_shutdown(void) {
    JNIEnv *env=text.env;
    if (env) {
        for (int i=0;i<FVG_FONT_COUNT;i++) if (text.paint[i]) (*env)->DeleteGlobalRef(env,text.paint[i]);
        if (text.config) (*env)->DeleteGlobalRef(env,text.config);
        jclass classes[]={text.paint_class,text.bitmap_class,text.canvas_class,text.rect_class};
        for (size_t i=0;i<sizeof(classes)/sizeof(classes[0]);i++) if (classes[i]) (*env)->DeleteGlobalRef(env,classes[i]);
        if (text.attached) (*java_vm)->DetachCurrentThread(java_vm);
    }
    memset(&text,0,sizeof(text));
}
#else
void fv_android_text_set_vm(void *vm) { (void)vm; }
bool fv_android_text_available(void) { return false; }
FvTextScale fv_android_text_scale(void *activity,float density) {
    (void)activity; FvTextScale scale;
    for (int i=0;i<FV_TEXT_SIZE_COUNT;i++) scale.px[i]=sizes[i]*density;
    return scale;
}
bool fv_android_text_glyph(int font,float pixels,uint32_t cp,FvTextGlyph *glyph) { (void)font; (void)pixels; (void)cp; (void)glyph; return false; }
bool fv_android_text_metrics(int font,float pixels,float *top,float *bottom) { (void)font; (void)pixels; (void)top; (void)bottom; return false; }
float fv_android_text_kerning(int font,float pixels,uint32_t left,uint32_t right) { (void)font; (void)pixels; (void)left; (void)right; return 0; }
void fv_android_text_shutdown(void) {}
#endif
