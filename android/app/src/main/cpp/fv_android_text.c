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
static bool checked(JNIEnv *env,const char *operation) {
    if (!(*env)->ExceptionCheck(env)) return true;
    (*env)->ExceptionClear(env);
    __android_log_print(ANDROID_LOG_WARN,"FieldVoxText","Platform font operation failed: %s",operation);
    return false;
}
static jclass global_class(JNIEnv *env,const char *name) {
    jclass local=(*env)->FindClass(env,name);
    if (!checked(env,name) || !local) return NULL;
    jclass global=(*env)->NewGlobalRef(env,local); (*env)->DeleteLocalRef(env,local);
    return checked(env,"font class reference") ? global : NULL;
}
static jmethodID method(JNIEnv *env,jclass cls,const char *name,const char *sig) {
    if (!cls) return NULL;
    jmethodID id=(*env)->GetMethodID(env,cls,name,sig);
    return checked(env,name) ? id : NULL;
}
static jfieldID field(JNIEnv *env,jclass cls,const char *name,const char *sig) {
    if (!cls) return NULL;
    jfieldID id=(*env)->GetFieldID(env,cls,name,sig);
    return checked(env,name) ? id : NULL;
}
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
    if ((*env)->PushLocalFrame(env,32)!=JNI_OK) { checked(env,"font initialization frame"); return false; }
    bool ok=false;
    text.paint_class=global_class(env,"android/graphics/Paint");
    text.bitmap_class=global_class(env,"android/graphics/Bitmap");
    text.canvas_class=global_class(env,"android/graphics/Canvas");
    text.rect_class=global_class(env,"android/graphics/Rect");
    jclass typeface=(*env)->FindClass(env,"android/graphics/Typeface");
    if (!checked(env,"Typeface") || !typeface) goto done;
    jclass config=(*env)->FindClass(env,"android/graphics/Bitmap$Config");
    if (!checked(env,"Bitmap Config") || !config) goto done;
    jclass fm=(*env)->FindClass(env,"android/graphics/Paint$FontMetrics");
    if (!checked(env,"FontMetrics") || !fm) goto done;
    if (!text.paint_class || !text.bitmap_class || !text.canvas_class || !text.rect_class) goto done;
#define M(member,cls,name,sig) text.member=method(env,cls,name,sig); if (!text.member) goto done
    M(size,text.paint_class,"setTextSize","(F)V");
    M(measure,text.paint_class,"measureText","(Ljava/lang/String;)F");
    M(measure_range,text.paint_class,"measureText","(Ljava/lang/String;II)F");
    M(bounds,text.paint_class,"getTextBounds","(Ljava/lang/String;IILandroid/graphics/Rect;)V");
    M(metrics,text.paint_class,"getFontMetrics","()Landroid/graphics/Paint$FontMetrics;");
    M(canvas_new,text.canvas_class,"<init>","(Landroid/graphics/Bitmap;)V");
    M(draw,text.canvas_class,"drawText","(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
    M(rect_new,text.rect_class,"<init>","()V");
#undef M
    text.left=field(env,text.rect_class,"left","I"); text.top=field(env,text.rect_class,"top","I");
    text.right=field(env,text.rect_class,"right","I"); text.bottom=field(env,text.rect_class,"bottom","I");
    text.metric_top=field(env,fm,"top","F"); text.metric_bottom=field(env,fm,"bottom","F");
    if (!text.left || !text.top || !text.right || !text.bottom || !text.metric_top || !text.metric_bottom) goto done;
    text.bitmap_new=(*env)->GetStaticMethodID(env,text.bitmap_class,"createBitmap","(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    if (!checked(env,"createBitmap") || !text.bitmap_new) goto done;
    const jfieldID argb=(*env)->GetStaticFieldID(env,config,"ARGB_8888","Landroid/graphics/Bitmap$Config;");
    if (!checked(env,"ARGB8888 field") || !argb) goto done;
    jobject cfg=(*env)->GetStaticObjectField(env,config,argb);
    if (!checked(env,"ARGB8888") || !cfg) goto done;
    text.config=(*env)->NewGlobalRef(env,cfg);
    if (!checked(env,"bitmap config reference") || !text.config) goto done;
    jmethodID constructor=method(env,text.paint_class,"<init>","(I)V");
    jmethodID face=method(env,text.paint_class,"setTypeface","(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;");
    jmethodID color=method(env,text.paint_class,"setColor","(I)V");
    jmethodID create=(*env)->GetStaticMethodID(env,typeface,"create","(Ljava/lang/String;I)Landroid/graphics/Typeface;");
    if (!checked(env,"Typeface.create") || !constructor || !face || !color || !create) goto done;
    for (int i=0;i<FVG_FONT_COUNT;i++) {
        jstring family=(*env)->NewStringUTF(env,i==FVG_FONT_MONO ? "monospace" : (i==FVG_FONT_MEDIUM ? "sans-serif-medium" : "sans-serif"));
        if (!checked(env,"font family") || !family) goto done;
        jobject font=(*env)->CallStaticObjectMethod(env,typeface,create,family,i==FVG_FONT_BOLD ? 1 : 0);
        if (!checked(env,"font face") || !font) goto done;
        jobject paint=(*env)->NewObject(env,text.paint_class,constructor,129); /* anti-alias + subpixel text */
        if (!checked(env,"Paint constructor") || !paint) goto done;
        (*env)->CallObjectMethod(env,paint,face,font);
        if (!checked(env,"font assignment")) goto done;
        (*env)->CallVoidMethod(env,paint,color,(jint)-1);
        if (!checked(env,"glyph color")) goto done;
        text.paint[i]=(*env)->NewGlobalRef(env,paint);
        if (!checked(env,"paint reference") || !text.paint[i]) goto done;
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
    if (!checked(env,"Paint size")) return false;
    jobject fm=(*env)->CallObjectMethod(env,text.paint[font],text.metrics);
    if (!checked(env,"font metrics") || !fm) return false;
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
    if ((*env)->PushLocalFrame(env,16)!=JNI_OK) { checked(env,"sp frame"); return scale; }
    jclass context=(*env)->FindClass(env,"android/content/Context");
    if (!checked(env,"Context") || !context) goto done;
    jclass resources=(*env)->FindClass(env,"android/content/res/Resources");
    if (!checked(env,"Resources") || !resources) goto done;
    jclass value=(*env)->FindClass(env,"android/util/TypedValue");
    if (!checked(env,"TypedValue") || !value) goto done;
    jmethodID get_resources=method(env,context,"getResources","()Landroid/content/res/Resources;");
    jmethodID get_metrics=method(env,resources,"getDisplayMetrics","()Landroid/util/DisplayMetrics;");
    jmethodID dimension=(*env)->GetStaticMethodID(env,value,"applyDimension","(IFLandroid/util/DisplayMetrics;)F");
    if (!checked(env,"applyDimension") || !get_resources || !get_metrics || !dimension) goto done;
    jobject res=(*env)->CallObjectMethod(env,activity,get_resources);
    if (!checked(env,"getResources") || !res) goto done;
    jobject metrics=(*env)->CallObjectMethod(env,res,get_metrics);
    if (!checked(env,"getDisplayMetrics") || !metrics) goto done;
    for (int i=0;i<FV_TEXT_SIZE_COUNT;i++) {
        const float px=(*env)->CallStaticFloatMethod(env,value,dimension,2,sizes[i],metrics);
        if (!checked(env,"sp conversion")) break;
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
    if ((*env)->PushLocalFrame(env,16)!=JNI_OK) { checked(env,"glyph frame"); return false; }
    bool ok=false; *out=(FvTextGlyph){0}; jchar chars[2]; const int length=utf16(cp,chars);
    jstring string=(*env)->NewString(env,chars,length);
    if (!checked(env,"glyph string") || !string) goto done;
    out->advance=(*env)->CallFloatMethod(env,text.paint[font],text.measure,string);
    if (!checked(env,"glyph advance")) goto done;
    jobject rect=(*env)->NewObject(env,text.rect_class,text.rect_new);
    if (!checked(env,"glyph rect") || !rect) goto done;
    (*env)->CallVoidMethod(env,text.paint[font],text.bounds,string,0,length,rect);
    if (!checked(env,"glyph bounds")) goto done;
    const int left=(*env)->GetIntField(env,rect,text.left),top=(*env)->GetIntField(env,rect,text.top);
    const int right=(*env)->GetIntField(env,rect,text.right),bottom=(*env)->GetIntField(env,rect,text.bottom);
    if (right<=left || bottom<=top) { ok=true; goto done; }
    if (right-left>1024 || bottom-top>1024) goto done;
    out->w=right-left+4; out->h=bottom-top+4; out->xoff=left-2; out->yoff=top-2;
    jobject bitmap=(*env)->CallStaticObjectMethod(env,text.bitmap_class,text.bitmap_new,out->w,out->h,text.config);
    if (!checked(env,"glyph bitmap") || !bitmap) goto done;
    jobject canvas=(*env)->NewObject(env,text.canvas_class,text.canvas_new,bitmap);
    if (!checked(env,"glyph canvas") || !canvas) goto done;
    (*env)->CallVoidMethod(env,canvas,text.draw,string,(float)-out->xoff,(float)-out->yoff,text.paint[font]);
    if (!checked(env,"glyph draw")) goto done;
    AndroidBitmapInfo info; void *pixels_data=NULL;
    if (AndroidBitmap_getInfo(env,bitmap,&info)!=ANDROID_BITMAP_RESULT_SUCCESS || info.format!=ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        info.width!=(uint32_t)out->w || info.height!=(uint32_t)out->h || info.stride<info.width*4u) goto done;
    out->bitmap=malloc((size_t)out->w*(size_t)out->h); if (!out->bitmap) goto done;
    if (AndroidBitmap_lockPixels(env,bitmap,&pixels_data)!=ANDROID_BITMAP_RESULT_SUCCESS) goto done;
    for (int y=0;y<out->h;y++) for (int x=0;x<out->w;x++)
        out->bitmap[(size_t)y*(size_t)out->w+(size_t)x]=((const uint8_t *)pixels_data)[(size_t)y*info.stride+(size_t)x*4u+3u];
    ok=AndroidBitmap_unlockPixels(env,bitmap)==ANDROID_BITMAP_RESULT_SUCCESS;
done:
    if (!ok) { checked(env,"glyph rasterization"); free(out->bitmap); *out=(FvTextGlyph){0}; }
    (*env)->PopLocalFrame(env,NULL); return ok;
}
float fv_android_text_kerning(int font,float pixels,uint32_t left,uint32_t right) {
    if (!set_size(font,pixels)) return 0;
    JNIEnv *env=text.env; jchar chars[4]; int first=utf16(left,chars); const int count=first+utf16(right,chars+first);
    jstring pair=(*env)->NewString(env,chars,count);
    if (!checked(env,"kerning string") || !pair) return 0;
    const float both=(*env)->CallFloatMethod(env,text.paint[font],text.measure,pair);
    if (!checked(env,"pair advance")) { (*env)->DeleteLocalRef(env,pair); return 0; }
    const float a=(*env)->CallFloatMethod(env,text.paint[font],text.measure_range,pair,0,first);
    if (!checked(env,"left advance")) { (*env)->DeleteLocalRef(env,pair); return 0; }
    const float b=(*env)->CallFloatMethod(env,text.paint[font],text.measure_range,pair,first,count);
    const bool ok=checked(env,"right advance"); (*env)->DeleteLocalRef(env,pair);
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
