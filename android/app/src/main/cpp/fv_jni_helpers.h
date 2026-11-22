/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_JNI_HELPERS_H
#define FV_JNI_HELPERS_H

#include <android/log.h>
#include <jni.h>
#include <stdbool.h>
#include <stddef.h>

#if __STDC_VERSION__ < 202311L
#error FieldVox requires C23
#endif

static inline bool fv_jni_check(JNIEnv *env, const char *log_tag, const char *operation, bool describe) {
    if (!(*env)->ExceptionCheck(env)) return true;
    __android_log_print(ANDROID_LOG_WARN, log_tag, "JNI operation failed: %s", operation);
    if (describe) (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return false;
}

static inline jclass fv_jni_find_class(JNIEnv *env, const char *log_tag, const char *name, bool describe) {
    jclass cls = (*env)->FindClass(env, name);
    return fv_jni_check(env, log_tag, name, describe) ? cls : NULL;
}

static inline jclass fv_jni_global_class(JNIEnv *env, const char *log_tag, const char *name, bool describe) {
    jclass local = fv_jni_find_class(env, log_tag, name, describe);
    if (!local) return NULL;
    jclass global = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    return fv_jni_check(env, log_tag, "class reference", describe) ? global : NULL;
}

static inline jmethodID fv_jni_method(JNIEnv *env, const char *log_tag, jclass cls,
                                      const char *name, const char *sig, bool describe) {
    if (!cls) return NULL;
    jmethodID id = (*env)->GetMethodID(env, cls, name, sig);
    return fv_jni_check(env, log_tag, name, describe) ? id : NULL;
}

static inline jfieldID fv_jni_field(JNIEnv *env, const char *log_tag, jclass cls,
                                    const char *name, const char *sig, bool describe) {
    if (!cls) return NULL;
    jfieldID id = (*env)->GetFieldID(env, cls, name, sig);
    return fv_jni_check(env, log_tag, name, describe) ? id : NULL;
}

#endif
