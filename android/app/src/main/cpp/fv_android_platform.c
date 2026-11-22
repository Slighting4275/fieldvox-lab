/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_android_platform.h"
#include "fv_jni_helpers.h"

#include <jni.h>

static JNIEnv *fv_get_env(ANativeActivity *activity, bool *attached) {
    if (!activity || !activity->vm) return NULL;
    *attached = false;
    JNIEnv *env = NULL;
    const jint status = (*activity->vm)->GetEnv(activity->vm, (void **)&env, JNI_VERSION_1_6);
    if (status == JNI_OK) return env;
    if ((*activity->vm)->AttachCurrentThread(activity->vm, &env, NULL) != JNI_OK) return NULL;
    *attached = true;
    return env;
}

bool fv_android_has_permission(ANativeActivity *activity, const char *permission) {
    bool attached = false;
    JNIEnv *env = fv_get_env(activity, &attached);
    if (!env || !permission) return false;
    jclass cls = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID method = fv_jni_method(env, NULL, cls, "checkSelfPermission", "(Ljava/lang/String;)I", false);
    jstring value = (*env)->NewStringUTF(env, permission);
    jint result = method && value ? (*env)->CallIntMethod(env, activity->clazz, method, value) : -1;
    fv_jni_clear(env);
    if (value) (*env)->DeleteLocalRef(env, value);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (attached) (*activity->vm)->DetachCurrentThread(activity->vm);
    return result == 0;
}

void fv_android_request_permissions(ANativeActivity *activity, const char * const *permissions,
                                    int count, int request_code) {
    if (!activity || !permissions || count <= 0) return;
    bool attached = false;
    JNIEnv *env = fv_get_env(activity, &attached);
    if (!env) return;
    jclass cls = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID request = fv_jni_method(env, NULL, cls, "requestPermissions", "([Ljava/lang/String;I)V", false);
    jclass string_cls = fv_jni_find_class(env, NULL, "java/lang/String", false);
    if (request && string_cls) {
        jobjectArray array = (*env)->NewObjectArray(env, count, string_cls, NULL);
        if (array) {
            for (int i = 0; i < count; i++) {
                if (!permissions[i]) continue;
                jstring str = (*env)->NewStringUTF(env, permissions[i]);
                if (str) {
                    (*env)->SetObjectArrayElement(env, array, i, str);
                    (*env)->DeleteLocalRef(env, str);
                }
            }
            (*env)->CallVoidMethod(env, activity->clazz, request, array, request_code);
            (*env)->DeleteLocalRef(env, array);
        }
    }
    fv_jni_check(env, "FieldVoxPlatform", "runtime permission request", false);
    if (string_cls) (*env)->DeleteLocalRef(env, string_cls);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (attached) (*activity->vm)->DetachCurrentThread(activity->vm);
}

void fv_android_request_permission(ANativeActivity *activity, const char *permission, int request_code) {
    if (!permission) return;
    fv_android_request_permissions(activity, &permission, 1, request_code);
}

void fv_android_request_runtime_permissions(ANativeActivity *activity) {
    const char *missing[2];
    int count = 0;
    if (!fv_android_has_permission(activity, "android.permission.RECORD_AUDIO")) {
        missing[count++] = "android.permission.RECORD_AUDIO";
    }
    if (!fv_android_has_permission(activity, "android.permission.CAMERA")) {
        missing[count++] = "android.permission.CAMERA";
    }
    if (count > 0) fv_android_request_permissions(activity, missing, count, 7);
}

static void fv_android_vibrate_effect(ANativeActivity *activity, int ms, bool double_pulse) {
    if (!activity || !activity->clazz || (!double_pulse && ms <= 0)) return;
    bool attached = false;
    JNIEnv *env = fv_get_env(activity, &attached);
    if (!env) return;
    jclass act_cls = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID get_sys_svc = fv_jni_method(env, NULL, act_cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", false);
    jstring vib_svc_str = (*env)->NewStringUTF(env, "vibrator");
    jobject vibrator = get_sys_svc && vib_svc_str ? (*env)->CallObjectMethod(env, activity->clazz, get_sys_svc, vib_svc_str) : NULL;
    if (vibrator) {
        jclass vib_cls = (*env)->GetObjectClass(env, vibrator);
        jclass effect_cls = fv_jni_find_class(env, NULL, "android/os/VibrationEffect", false);
        if (effect_cls) {
            jobject effect = NULL;
            if (double_pulse) {
                jlongArray timings = (*env)->NewLongArray(env, 4);
                jlong tvals[4] = {0, 35, 55, 45};
                (*env)->SetLongArrayRegion(env, timings, 0, 4, tvals);
                jmethodID create = fv_jni_static_method(
                    env, NULL, effect_cls, "createWaveform", "([JI)Landroid/os/VibrationEffect;", false);
                if (create && timings) effect = (*env)->CallStaticObjectMethod(env, effect_cls, create, timings, -1);
                if (timings) (*env)->DeleteLocalRef(env, timings);
            } else {
                jmethodID create = fv_jni_static_method(
                    env, NULL, effect_cls, "createOneShot", "(JI)Landroid/os/VibrationEffect;", false);
                if (create) effect = (*env)->CallStaticObjectMethod(env, effect_cls, create, (jlong)(ms > 0 ? ms : 15), -1);
            }
            if (effect) {
                jmethodID vibrate = fv_jni_method(
                    env, NULL, vib_cls, "vibrate", "(Landroid/os/VibrationEffect;)V", false);
                if (vibrate) (*env)->CallVoidMethod(env, vibrator, vibrate, effect);
                (*env)->DeleteLocalRef(env, effect);
            }
            (*env)->DeleteLocalRef(env, effect_cls);
        }
        (*env)->DeleteLocalRef(env, vib_cls);
        (*env)->DeleteLocalRef(env, vibrator);
    }
    fv_jni_clear(env);
    if (vib_svc_str) (*env)->DeleteLocalRef(env, vib_svc_str);
    if (act_cls) (*env)->DeleteLocalRef(env, act_cls);
    if (attached) (*activity->vm)->DetachCurrentThread(activity->vm);
}

void fv_android_vibrate(ANativeActivity *activity, int ms) {
    fv_android_vibrate_effect(activity, ms, false);
}

void fv_android_vibrate_double(ANativeActivity *activity) {
    fv_android_vibrate_effect(activity, 0, true);
}

int fv_android_get_display_rotation_degrees(ANativeActivity *activity) {
    if (!activity || !activity->vm) return 0;
    bool attached = false;
    JNIEnv *env = fv_get_env(activity, &attached);
    if (!env) return 0;

    jint rot_val = 0;
    jclass act_cls = (*env)->GetObjectClass(env, activity->clazz);
    if (act_cls) {
        jmethodID get_wm = fv_jni_method(env, NULL, act_cls, "getWindowManager", "()Landroid/view/WindowManager;", false);
        if (get_wm) {
            jobject wm = (*env)->CallObjectMethod(env, activity->clazz, get_wm);
            if (wm) {
                jclass wm_cls = (*env)->GetObjectClass(env, wm);
                jmethodID get_disp = fv_jni_method(env, NULL, wm_cls, "getDefaultDisplay", "()Landroid/view/Display;", false);
                if (get_disp) {
                    jobject disp = (*env)->CallObjectMethod(env, wm, get_disp);
                    if (disp) {
                        jclass disp_cls = (*env)->GetObjectClass(env, disp);
                        jmethodID get_rot = fv_jni_method(env, NULL, disp_cls, "getRotation", "()I", false);
                        if (get_rot) rot_val = (*env)->CallIntMethod(env, disp, get_rot);
                        if (disp_cls) (*env)->DeleteLocalRef(env, disp_cls);
                        (*env)->DeleteLocalRef(env, disp);
                    }
                }
                if (wm_cls) (*env)->DeleteLocalRef(env, wm_cls);
                (*env)->DeleteLocalRef(env, wm);
            }
        }
        (*env)->DeleteLocalRef(env, act_cls);
    }
    fv_jni_clear(env);
    if (attached) (*activity->vm)->DetachCurrentThread(activity->vm);

    switch (rot_val) {
        case 1: return 90;
        case 2: return 180;
        case 3: return 270;
        default: return 0;
    }
}
