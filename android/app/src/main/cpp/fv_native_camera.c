#include "fv_native_camera.h"
#include "fv_recording_data.h"

#include <android/log.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadataTags.h>
#include <jni.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FV_CAM_TAG "FieldVoxCamera"
#define CAM_LOGI(...) __android_log_print(ANDROID_LOG_INFO, FV_CAM_TAG, __VA_ARGS__)
#define CAM_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, FV_CAM_TAG, __VA_ARGS__)

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t callbacks_idle;
    ACameraManager *manager;
    ACameraDevice *device;
    ACameraCaptureSession *session;
    ACaptureRequest *request;
    ACameraOutputTarget *target;
    ACaptureSessionOutputContainer *outputs;
    ACaptureSessionOutput *output;
    AImageReader *reader;
    ANativeWindow *reader_window;
    uint32_t argb[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT];
    bool frame_ready;
    bool active;
    bool failed;
    int sensor_orientation;
    uint64_t last_frame_ms;
    bool torch_on;
    uint64_t generation;
    uint32_t image_callbacks;
    AImageReader *callback_reader;
} FvCameraState;

static FvCameraState s_camera = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .callbacks_idle = PTHREAD_COND_INITIALIZER,
    .sensor_orientation = 90,
};
static pthread_mutex_t s_camera_control_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t current_camera_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static uint64_t fv_camera_next_generation(AImageReader *reader) {
    pthread_mutex_lock(&s_camera.mutex);
    uint64_t generation = ++s_camera.generation;
    if (generation == 0) generation = ++s_camera.generation;
    s_camera.callback_reader = reader;
    pthread_mutex_unlock(&s_camera.mutex);
    return generation;
}

static bool fv_camera_begin_image_callback(void *context, AImageReader *reader) {
    const uint64_t generation = (uint64_t)(uintptr_t)context;
    pthread_mutex_lock(&s_camera.mutex);
    const bool current = generation != 0 && generation == s_camera.generation &&
                         reader != NULL && reader == s_camera.callback_reader;
    if (current) s_camera.image_callbacks++;
    pthread_mutex_unlock(&s_camera.mutex);
    return current;
}

static void fv_camera_end_image_callback(void) {
    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.image_callbacks > 0) s_camera.image_callbacks--;
    if (s_camera.image_callbacks == 0) pthread_cond_broadcast(&s_camera.callbacks_idle);
    pthread_mutex_unlock(&s_camera.mutex);
}

static bool fv_camera_callback_is_current(void *context) {
    const uint64_t generation = (uint64_t)(uintptr_t)context;
    pthread_mutex_lock(&s_camera.mutex);
    const bool current = generation != 0 && generation == s_camera.generation;
    pthread_mutex_unlock(&s_camera.mutex);
    return current;
}

/* Quiesce asynchronous image callbacks before invalidating their reader and image data. */
static void fv_camera_destroy_graph(ACameraManager *manager, ACameraDevice *device,
                                    ACameraCaptureSession *session, ACaptureRequest *request,
                                    ACameraOutputTarget *target, ACaptureSessionOutput *output,
                                    ACaptureSessionOutputContainer *outputs, AImageReader *reader) {
    (void)manager; /* Process-lifetime singleton; watchdog restarts must not churn/delete it. */
    if (session) ACameraCaptureSession_stopRepeating(session);
    if (reader) AImageReader_setImageListener(reader, NULL);

    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.callback_reader == reader) s_camera.callback_reader = NULL;
    while (s_camera.image_callbacks != 0) {
        pthread_cond_wait(&s_camera.callbacks_idle, &s_camera.mutex);
    }
    pthread_mutex_unlock(&s_camera.mutex);

    if (session) ACameraCaptureSession_close(session);
    /* Device close drains in-flight captures before request graph resources are released. */
    if (device) ACameraDevice_close(device);
    if (request) ACaptureRequest_free(request);
    if (target) ACameraOutputTarget_free(target);
    if (output) ACaptureSessionOutput_free(output);
    if (outputs) ACaptureSessionOutputContainer_free(outputs);
    if (reader) AImageReader_delete(reader);
}

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
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, "checkSelfPermission", "(Ljava/lang/String;)I") : NULL;
    jstring value = (*env)->NewStringUTF(env, permission);
    const jint result = method && value ? (*env)->CallIntMethod(env, activity->clazz, method, value) : -1;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (value) (*env)->DeleteLocalRef(env, value);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (attached) (*activity->vm)->DetachCurrentThread(activity->vm);
    return result == 0;
}

void fv_android_request_permissions(ANativeActivity *activity, const char * const *permissions, int count, int request_code) {
    bool attached = false;
    JNIEnv *env = fv_get_env(activity, &attached);
    if (!env || !permissions || count <= 0) return;
    jclass cls = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID request = cls ? (*env)->GetMethodID(env, cls, "requestPermissions", "([Ljava/lang/String;I)V") : NULL;
    jclass string_cls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray values = string_cls ? (*env)->NewObjectArray(env, count, string_cls, NULL) : NULL;
    if (request && values) {
        for (int i = 0; i < count; i++) {
            if (permissions[i]) {
                jstring val = (*env)->NewStringUTF(env, permissions[i]);
                (*env)->SetObjectArrayElement(env, values, i, val);
                if (val) (*env)->DeleteLocalRef(env, val);
            }
        }
        (*env)->CallVoidMethod(env, activity->clazz, request, values, request_code);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        CAM_LOGE("Runtime permission request failed");
    }
    if (values) (*env)->DeleteLocalRef(env, values);
    if (string_cls) (*env)->DeleteLocalRef(env, string_cls);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (attached) (*activity->vm)->DetachCurrentThread(activity->vm);
}

void fv_android_request_permission(ANativeActivity *activity, const char *permission, int request_code) {
    if (!permission) return;
    const char *perms[1] = { permission };
    fv_android_request_permissions(activity, perms, 1, request_code);
}

void fv_android_request_runtime_permissions(ANativeActivity *activity) {
    const char *perms[2] = {
        "android.permission.RECORD_AUDIO",
        "android.permission.CAMERA"
    };
    fv_android_request_permissions(activity, perms, 2, 60);
}

static void fv_android_vibrate_effect(ANativeActivity *activity, int ms, bool double_pulse) {
    if (!activity || !activity->vm || (!double_pulse && ms <= 0)) return;
    bool attached = false;
    JNIEnv *env = fv_get_env(activity, &attached);
    if (!env) return;
    jclass act_cls = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID get_sys_svc = act_cls ? (*env)->GetMethodID(env, act_cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
    jstring svc_name = (*env)->NewStringUTF(env, "vibrator");
    jobject vibrator = (get_sys_svc && svc_name) ? (*env)->CallObjectMethod(env, activity->clazz, get_sys_svc, svc_name) : NULL;
    if (vibrator) {
        jclass vib_cls = (*env)->GetObjectClass(env, vibrator);
        jclass effect_cls = (*env)->FindClass(env, "android/os/VibrationEffect");
        jobject effect = NULL;
        if (effect_cls && double_pulse) {
            const jlong timings[] = {0, 35, 55, 45};
            jlongArray pattern = (*env)->NewLongArray(env, 4);
            if (pattern) {
                (*env)->SetLongArrayRegion(env, pattern, 0, 4, timings);
                jmethodID create = (*env)->GetStaticMethodID(
                    env, effect_cls, "createWaveform", "([JI)Landroid/os/VibrationEffect;");
                if (create) effect = (*env)->CallStaticObjectMethod(env, effect_cls, create, pattern, -1);
                (*env)->DeleteLocalRef(env, pattern);
            }
        } else if (effect_cls) {
            jmethodID create = (*env)->GetStaticMethodID(
                env, effect_cls, "createOneShot", "(JI)Landroid/os/VibrationEffect;");
            if (create) effect = (*env)->CallStaticObjectMethod(env, effect_cls, create, (jlong)ms, -1);
        }
        if (effect && vib_cls) {
            jmethodID vibrate = (*env)->GetMethodID(
                env, vib_cls, "vibrate", "(Landroid/os/VibrationEffect;)V");
            if (vibrate) (*env)->CallVoidMethod(env, vibrator, vibrate, effect);
        }
        if (effect) (*env)->DeleteLocalRef(env, effect);
        if (effect_cls) (*env)->DeleteLocalRef(env, effect_cls);
        if (vib_cls) (*env)->DeleteLocalRef(env, vib_cls);
        (*env)->DeleteLocalRef(env, vibrator);
    }
    if (svc_name) (*env)->DeleteLocalRef(env, svc_name);
    if (act_cls) (*env)->DeleteLocalRef(env, act_cls);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*activity->vm)->DetachCurrentThread(activity->vm);
}

void fv_android_vibrate(ANativeActivity *activity, int ms) {
    fv_android_vibrate_effect(activity, ms, false);
}

void fv_android_vibrate_double(ANativeActivity *activity) {
    fv_android_vibrate_effect(activity, 0, true);
}

typedef struct {
    JavaVM *vm;
    jobject activity;
    char path[PATH_MAX];
    char mime[64];
} FvOpenFileJob;

static bool fv_copy_file_to_fd(const char *path, int dst_fd) {
    int src_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) return false;
    bool ok = true;
    uint8_t buffer[64 * 1024];
    for (;;) {
        ssize_t nr = read(src_fd, buffer, sizeof(buffer));
        if (nr == 0) break;
        if (nr < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        ssize_t offset = 0;
        while (offset < nr) {
            ssize_t nw = write(dst_fd, buffer + offset, (size_t)(nr - offset));
            if (nw < 0 && errno == EINTR) continue;
            if (nw <= 0) {
                ok = false;
                break;
            }
            offset += nw;
        }
        if (!ok) break;
    }
    if (close(src_fd) != 0) ok = false;
    return ok;
}

static jobject fv_find_published_media(JNIEnv *env, jobject resolver, jobject collection,
                                       const char *name, const char *relative_path) {
    jclass resolver_cls = (*env)->GetObjectClass(env, resolver);
    jclass string_cls = (*env)->FindClass(env, "java/lang/String");
    jclass content_uris_cls = (*env)->FindClass(env, "android/content/ContentUris");
    jobject result = NULL;
    jobjectArray projection = string_cls ? (*env)->NewObjectArray(env, 1, string_cls, NULL) : NULL;
    jobjectArray args = string_cls ? (*env)->NewObjectArray(env, 2, string_cls, NULL) : NULL;
    jstring id_column = (*env)->NewStringUTF(env, "_id");
    jstring display_name = (*env)->NewStringUTF(env, name);
    jstring relative = (*env)->NewStringUTF(env, relative_path);
    jstring selection = (*env)->NewStringUTF(env, "_display_name=? AND relative_path=?");
    if (projection && args && id_column && display_name && relative) {
        (*env)->SetObjectArrayElement(env, projection, 0, id_column);
        (*env)->SetObjectArrayElement(env, args, 0, display_name);
        (*env)->SetObjectArrayElement(env, args, 1, relative);
        jmethodID query = resolver_cls ? (*env)->GetMethodID(
            env, resolver_cls, "query",
            "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;") : NULL;
        jobject cursor = query ? (*env)->CallObjectMethod(
            env, resolver, query, collection, projection, selection, args, NULL) : NULL;
        if (cursor && !(*env)->ExceptionCheck(env)) {
            jclass cursor_cls = (*env)->GetObjectClass(env, cursor);
            jmethodID move_first = cursor_cls ? (*env)->GetMethodID(env, cursor_cls, "moveToFirst", "()Z") : NULL;
            jmethodID get_long = cursor_cls ? (*env)->GetMethodID(env, cursor_cls, "getLong", "(I)J") : NULL;
            if (move_first && get_long && (*env)->CallBooleanMethod(env, cursor, move_first)) {
                jlong id = (*env)->CallLongMethod(env, cursor, get_long, 0);
                jmethodID append_id = content_uris_cls ? (*env)->GetStaticMethodID(
                    env, content_uris_cls, "withAppendedId",
                    "(Landroid/net/Uri;J)Landroid/net/Uri;") : NULL;
                if (append_id) result = (*env)->CallStaticObjectMethod(env, content_uris_cls, append_id, collection, id);
            }
            jmethodID close_cursor = cursor_cls ? (*env)->GetMethodID(env, cursor_cls, "close", "()V") : NULL;
            if (close_cursor) (*env)->CallVoidMethod(env, cursor, close_cursor);
            if (cursor_cls) (*env)->DeleteLocalRef(env, cursor_cls);
            (*env)->DeleteLocalRef(env, cursor);
        }
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        result = NULL;
    }
    if (selection) (*env)->DeleteLocalRef(env, selection);
    if (relative) (*env)->DeleteLocalRef(env, relative);
    if (display_name) (*env)->DeleteLocalRef(env, display_name);
    if (id_column) (*env)->DeleteLocalRef(env, id_column);
    if (args) (*env)->DeleteLocalRef(env, args);
    if (projection) (*env)->DeleteLocalRef(env, projection);
    if (content_uris_cls) (*env)->DeleteLocalRef(env, content_uris_cls);
    if (string_cls) (*env)->DeleteLocalRef(env, string_cls);
    if (resolver_cls) (*env)->DeleteLocalRef(env, resolver_cls);
    return result;
}

static jobject fv_publish_media(JNIEnv *env, jobject resolver, jobject collection,
                                const char *source_path, const char *name,
                                const char *mime, const char *relative_path) {
    jobject existing = fv_find_published_media(env, resolver, collection, name, relative_path);
    if (existing) return existing;

    jclass resolver_cls = (*env)->GetObjectClass(env, resolver);
    jclass values_cls = (*env)->FindClass(env, "android/content/ContentValues");
    jclass integer_cls = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID values_init = values_cls ? (*env)->GetMethodID(env, values_cls, "<init>", "()V") : NULL;
    jobject values = values_init ? (*env)->NewObject(env, values_cls, values_init) : NULL;
    jmethodID put_string = values_cls ? (*env)->GetMethodID(
        env, values_cls, "put", "(Ljava/lang/String;Ljava/lang/String;)V") : NULL;
    jmethodID put_integer = values_cls ? (*env)->GetMethodID(
        env, values_cls, "put", "(Ljava/lang/String;Ljava/lang/Integer;)V") : NULL;
    jmethodID integer_value_of = integer_cls ? (*env)->GetStaticMethodID(
        env, integer_cls, "valueOf", "(I)Ljava/lang/Integer;") : NULL;
    jstring key_name = (*env)->NewStringUTF(env, "_display_name");
    jstring key_mime = (*env)->NewStringUTF(env, "mime_type");
    jstring key_path = (*env)->NewStringUTF(env, "relative_path");
    jstring key_pending = (*env)->NewStringUTF(env, "is_pending");
    jstring val_name = (*env)->NewStringUTF(env, name);
    jstring val_mime = (*env)->NewStringUTF(env, mime);
    jstring val_path = (*env)->NewStringUTF(env, relative_path);
    jobject one = integer_value_of ? (*env)->CallStaticObjectMethod(env, integer_cls, integer_value_of, 1) : NULL;
    if (values && put_string && put_integer && key_name && key_mime && key_path && key_pending &&
        val_name && val_mime && val_path && one) {
        (*env)->CallVoidMethod(env, values, put_string, key_name, val_name);
        (*env)->CallVoidMethod(env, values, put_string, key_mime, val_mime);
        (*env)->CallVoidMethod(env, values, put_string, key_path, val_path);
        (*env)->CallVoidMethod(env, values, put_integer, key_pending, one);
    }
    jmethodID insert = resolver_cls ? (*env)->GetMethodID(
        env, resolver_cls, "insert",
        "(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;") : NULL;
    jobject uri = (insert && values && !(*env)->ExceptionCheck(env)) ?
        (*env)->CallObjectMethod(env, resolver, insert, collection, values) : NULL;

    bool copied = false;
    if (uri && !(*env)->ExceptionCheck(env)) {
        jstring write_mode = (*env)->NewStringUTF(env, "w");
        jmethodID open_pfd = resolver_cls ? (*env)->GetMethodID(
            env, resolver_cls, "openFileDescriptor",
            "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;") : NULL;
        jobject pfd = (open_pfd && write_mode) ?
            (*env)->CallObjectMethod(env, resolver, open_pfd, uri, write_mode) : NULL;
        if (pfd && !(*env)->ExceptionCheck(env)) {
            jclass pfd_cls = (*env)->GetObjectClass(env, pfd);
            jmethodID detach_fd = pfd_cls ? (*env)->GetMethodID(env, pfd_cls, "detachFd", "()I") : NULL;
            int fd = detach_fd ? (*env)->CallIntMethod(env, pfd, detach_fd) : -1;
            if (fd >= 0 && !(*env)->ExceptionCheck(env)) {
                copied = fv_copy_file_to_fd(source_path, fd);
                if (fsync(fd) != 0) copied = false;
                if (close(fd) != 0) copied = false;
            }
            if (pfd_cls) (*env)->DeleteLocalRef(env, pfd_cls);
            (*env)->DeleteLocalRef(env, pfd);
        }
        if (write_mode) (*env)->DeleteLocalRef(env, write_mode);
    }

    if (uri && copied && !(*env)->ExceptionCheck(env)) {
        jmethodID clear = values_cls ? (*env)->GetMethodID(env, values_cls, "clear", "()V") : NULL;
        jobject zero = integer_value_of ? (*env)->CallStaticObjectMethod(env, integer_cls, integer_value_of, 0) : NULL;
        if (clear && put_integer && zero) {
            (*env)->CallVoidMethod(env, values, clear);
            (*env)->CallVoidMethod(env, values, put_integer, key_pending, zero);
            jmethodID update = resolver_cls ? (*env)->GetMethodID(
                env, resolver_cls, "update",
                "(Landroid/net/Uri;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;)I") : NULL;
            if (update) (*env)->CallIntMethod(env, resolver, update, uri, values, NULL, NULL);
        }
        if (zero) (*env)->DeleteLocalRef(env, zero);
    } else if (uri) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        jmethodID delete_uri = resolver_cls ? (*env)->GetMethodID(
            env, resolver_cls, "delete", "(Landroid/net/Uri;Ljava/lang/String;[Ljava/lang/String;)I") : NULL;
        if (delete_uri) (*env)->CallIntMethod(env, resolver, delete_uri, uri, NULL, NULL);
        (*env)->DeleteLocalRef(env, uri);
        uri = NULL;
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (uri) (*env)->DeleteLocalRef(env, uri);
        uri = NULL;
    }
    if (one) (*env)->DeleteLocalRef(env, one);
    if (val_path) (*env)->DeleteLocalRef(env, val_path);
    if (val_mime) (*env)->DeleteLocalRef(env, val_mime);
    if (val_name) (*env)->DeleteLocalRef(env, val_name);
    if (key_pending) (*env)->DeleteLocalRef(env, key_pending);
    if (key_path) (*env)->DeleteLocalRef(env, key_path);
    if (key_mime) (*env)->DeleteLocalRef(env, key_mime);
    if (key_name) (*env)->DeleteLocalRef(env, key_name);
    if (values) (*env)->DeleteLocalRef(env, values);
    if (integer_cls) (*env)->DeleteLocalRef(env, integer_cls);
    if (values_cls) (*env)->DeleteLocalRef(env, values_cls);
    if (resolver_cls) (*env)->DeleteLocalRef(env, resolver_cls);
    return uri;
}

static void fv_launch_media_viewer(JNIEnv *env, jobject activity, jobject uri, const char *mime) {
    jclass intent_cls = (*env)->FindClass(env, "android/content/Intent");
    jclass activity_cls = (*env)->GetObjectClass(env, activity);
    jstring action_view = (*env)->NewStringUTF(env, "android.intent.action.VIEW");
    jstring jmime = (*env)->NewStringUTF(env, mime);
    jmethodID intent_init = (intent_cls && action_view) ? (*env)->GetMethodID(
        env, intent_cls, "<init>", "(Ljava/lang/String;)V") : NULL;
    jobject intent = intent_init ? (*env)->NewObject(env, intent_cls, intent_init, action_view) : NULL;
    if (intent && uri && jmime) {
        jmethodID set_data_type = (*env)->GetMethodID(
            env, intent_cls, "setDataAndType",
            "(Landroid/net/Uri;Ljava/lang/String;)Landroid/content/Intent;");
        jmethodID add_flags = (*env)->GetMethodID(
            env, intent_cls, "addFlags", "(I)Landroid/content/Intent;");
        jmethodID start_activity = activity_cls ? (*env)->GetMethodID(
            env, activity_cls, "startActivity", "(Landroid/content/Intent;)V") : NULL;
        if (set_data_type) (*env)->CallObjectMethod(env, intent, set_data_type, uri, jmime);
        if (add_flags) (*env)->CallObjectMethod(
            env, intent, add_flags, 0x10000000 | 0x00000001); /* NEW_TASK | GRANT_READ_URI_PERMISSION */
        if (start_activity) (*env)->CallVoidMethod(env, activity, start_activity, intent);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        CAM_LOGE("No system viewer could open the published evidence");
    }
    if (intent) (*env)->DeleteLocalRef(env, intent);
    if (jmime) (*env)->DeleteLocalRef(env, jmime);
    if (action_view) (*env)->DeleteLocalRef(env, action_view);
    if (activity_cls) (*env)->DeleteLocalRef(env, activity_cls);
    if (intent_cls) (*env)->DeleteLocalRef(env, intent_cls);
}

static void *fv_open_file_worker(void *opaque) {
    FvOpenFileJob *job = opaque;
    JNIEnv *env = NULL;
    if ((*job->vm)->AttachCurrentThread(job->vm, &env, NULL) != JNI_OK || !env) {
        free(job);
        return NULL;
    }
    jclass activity_cls = (*env)->GetObjectClass(env, job->activity);
    jmethodID get_resolver = activity_cls ? (*env)->GetMethodID(
        env, activity_cls, "getContentResolver", "()Landroid/content/ContentResolver;") : NULL;
    jobject resolver = get_resolver ? (*env)->CallObjectMethod(env, job->activity, get_resolver) : NULL;

    const bool video = strncmp(job->mime, "video/", 6) == 0;
    const char *media_class_name = video ?
        "android/provider/MediaStore$Video$Media" : "android/provider/MediaStore$Audio$Media";
    const char *relative_path = video ? "Movies/FieldVox/" : "Music/FieldVox/";
    jclass media_cls = (*env)->FindClass(env, media_class_name);
    jfieldID external_uri = media_cls ? (*env)->GetStaticFieldID(
        env, media_cls, "EXTERNAL_CONTENT_URI", "Landroid/net/Uri;") : NULL;
    jobject collection = external_uri ? (*env)->GetStaticObjectField(env, media_cls, external_uri) : NULL;
    const char *slash = strrchr(job->path, '/');
    const char *name = slash ? slash + 1 : job->path;
    jobject published = (resolver && collection && name[0] && !(*env)->ExceptionCheck(env)) ?
        fv_publish_media(env, resolver, collection, job->path, name, job->mime, relative_path) : NULL;
    if (published) {
        CAM_LOGI("Published evidence to MediaStore: %s", name);
        fv_launch_media_viewer(env, job->activity, published, job->mime);
    } else {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        CAM_LOGE("Failed to publish evidence: %s", job->path);
    }
    if (published) (*env)->DeleteLocalRef(env, published);
    if (collection) (*env)->DeleteLocalRef(env, collection);
    if (media_cls) (*env)->DeleteLocalRef(env, media_cls);
    if (resolver) (*env)->DeleteLocalRef(env, resolver);
    if (activity_cls) (*env)->DeleteLocalRef(env, activity_cls);
    (*env)->DeleteGlobalRef(env, job->activity);
    (*job->vm)->DetachCurrentThread(job->vm);
    free(job);
    return NULL;
}

void fv_android_open_file(ANativeActivity *activity, const char *path, const char *mime_type) {
    if (!activity || !activity->vm || !activity->clazz || !path || !path[0]) return;
    bool attached = false;
    JNIEnv *env = fv_get_env(activity, &attached);
    if (!env) return;
    FvOpenFileJob *job = calloc(1, sizeof(*job));
    if (job) {
        job->vm = activity->vm;
        job->activity = (*env)->NewGlobalRef(env, activity->clazz);
        snprintf(job->path, sizeof(job->path), "%s", path);
        snprintf(job->mime, sizeof(job->mime), "%s", mime_type && mime_type[0] ? mime_type : "application/octet-stream");
    }
    pthread_t thread;
    if (!job || !job->activity || pthread_create(&thread, NULL, fv_open_file_worker, job) != 0) {
        if (job && job->activity) (*env)->DeleteGlobalRef(env, job->activity);
        free(job);
        CAM_LOGE("Could not start evidence export worker");
    } else {
        pthread_detach(thread);
    }
    if (attached) (*activity->vm)->DetachCurrentThread(activity->vm);
}

static void fv_camera_image_available(void *context, AImageReader *reader) {
    if (!fv_camera_begin_image_callback(context, reader)) return;
    AImage *latest_image = NULL;
    AImage *image = NULL;

    /* Drain all pending images from the reader queue to return older buffers immediately to the HAL */
    while (AImageReader_acquireNextImage(reader, &image) == AMEDIA_OK && image) {
        if (latest_image) {
            AImage_delete(latest_image);
        }
        latest_image = image;
        image = NULL;
    }

    if (!latest_image) {
        fv_camera_end_image_callback();
        return;
    }

    const uint8_t *planes[3] = {0};
    size_t lengths[3] = {0};
    int strides[3] = {0}, pixels[3] = {0};
    int32_t width=0, height=0;
    AImageCropRect crop;
    bool valid=AImage_getWidth(latest_image,&width)==AMEDIA_OK &&
        AImage_getHeight(latest_image,&height)==AMEDIA_OK &&
        AImage_getCropRect(latest_image,&crop)==AMEDIA_OK;
    for (int i=0; valid && i<3; i++) {
        uint8_t *data=NULL;
        int length=0;
        int32_t stride=0, pixel=0;
        valid=AImage_getPlaneData(latest_image,i,&data,&length)==AMEDIA_OK && data && length>0 &&
            AImage_getPlaneRowStride(latest_image,i,&stride)==AMEDIA_OK &&
            AImage_getPlanePixelStride(latest_image,i,&pixel)==AMEDIA_OK;
        planes[i]=data; lengths[i]=length>0 ? (size_t)length : 0;
        strides[i]=stride; pixels[i]=pixel;
    }
    if (valid) {
        pthread_mutex_lock(&s_camera.mutex);
        /* Preserve chroma; the shared converter validates padding and interleaved planes. */
        if (fv_yuv420_to_argb(s_camera.argb,FV_CAMERA_WIDTH,FV_CAMERA_HEIGHT,width,height,
                             crop.left,crop.top,crop.right,crop.bottom,planes,lengths,strides,pixels)) {
            s_camera.frame_ready=true;
            s_camera.last_frame_ms=current_camera_time_ms();
        }
        pthread_mutex_unlock(&s_camera.mutex);
    }
    AImage_delete(latest_image);
    fv_camera_end_image_callback();
}

static void fv_camera_disconnected(void *context, ACameraDevice *device) {
    if (!fv_camera_callback_is_current(context)) return;
    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.device == device) {
        s_camera.active = false;
        s_camera.failed = true;
    }
    pthread_mutex_unlock(&s_camera.mutex);
    CAM_LOGE("Camera disconnected");
}

static void fv_camera_error(void *context, ACameraDevice *device, int error) {
    if (!fv_camera_callback_is_current(context)) return;
    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.device == device) {
        s_camera.active = false;
        s_camera.failed = true;
    }
    pthread_mutex_unlock(&s_camera.mutex);
    CAM_LOGE("Camera error: %d", error);
}

static void fv_session_closed(void *context, ACameraCaptureSession *session) {
    if (!fv_camera_callback_is_current(context)) return;
    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.session == session) {
        s_camera.active = false;
        s_camera.failed = true;
    }
    pthread_mutex_unlock(&s_camera.mutex);
}

static void fv_session_ready(void *context, ACameraCaptureSession *session) {
    if (!fv_camera_callback_is_current(context)) return;
    (void)session;
}

static void fv_session_active(void *context, ACameraCaptureSession *session) {
    if (!fv_camera_callback_is_current(context)) return;
    pthread_mutex_lock(&s_camera.mutex);
    const bool current_session = s_camera.session == session;
    if (current_session) {
        s_camera.active = true;
        s_camera.failed = false;
        s_camera.last_frame_ms = current_camera_time_ms();
    }
    pthread_mutex_unlock(&s_camera.mutex);
    if (current_session) CAM_LOGI("Native camera preview active");
}

bool fv_camera_start(void) {
    pthread_mutex_lock(&s_camera_control_mutex);
    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.device || s_camera.session || s_camera.reader) {
        const bool usable_or_starting = !s_camera.failed;
        pthread_mutex_unlock(&s_camera.mutex);
        pthread_mutex_unlock(&s_camera_control_mutex);
        return usable_or_starting;
    }
    pthread_mutex_unlock(&s_camera.mutex);

    ACameraManager *manager = s_camera.manager;
    if (!manager) {
        manager = ACameraManager_create();
        if (manager) {
            pthread_mutex_lock(&s_camera.mutex);
            s_camera.manager = manager;
            pthread_mutex_unlock(&s_camera.mutex);
        }
    }
    ACameraIdList *ids = NULL;
    if (!manager || ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK || !ids || ids->numCameras < 1) {
        if (ids) ACameraManager_deleteCameraIdList(ids);
        CAM_LOGE("No native camera is available");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }

    const char *selected = ids->cameraIds[0];
    int detected_orientation = 90;
    for (int i = 0; i < ids->numCameras; i++) {
        ACameraMetadata *metadata = NULL;
        ACameraMetadata_const_entry entry;
        if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &metadata) == ACAMERA_OK && metadata &&
            ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK &&
            entry.count > 0 && entry.data.u8[0] == ACAMERA_LENS_FACING_BACK) {
            selected = ids->cameraIds[i];
            ACameraMetadata_const_entry ori_entry;
            if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_ORIENTATION, &ori_entry) == ACAMERA_OK && ori_entry.count > 0) {
                detected_orientation = ori_entry.data.i32[0];
            }
            ACameraMetadata_free(metadata);
            break;
        }
        if (metadata) ACameraMetadata_free(metadata);
    }
    pthread_mutex_lock(&s_camera.mutex);
    s_camera.sensor_orientation = detected_orientation;
    pthread_mutex_unlock(&s_camera.mutex);
    CAM_LOGI("Selected camera '%s' (sensor orientation: %d deg)", selected, detected_orientation);
    char selected_id[64];
    snprintf(selected_id, sizeof(selected_id), "%s", selected);
    ACameraManager_deleteCameraIdList(ids);

    AImageReader *reader = NULL;
    if (AImageReader_new(FV_CAMERA_WIDTH, FV_CAMERA_HEIGHT, AIMAGE_FORMAT_YUV_420_888, 8, &reader) != AMEDIA_OK || !reader) {
        CAM_LOGE("AImageReader creation failed");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }
    const uint64_t generation = fv_camera_next_generation(reader);
    AImageReader_ImageListener listener = {
        .context = (void *)(uintptr_t)generation,
        .onImageAvailable = fv_camera_image_available,
    };
    if (AImageReader_setImageListener(reader, &listener) != AMEDIA_OK) {
        fv_camera_destroy_graph(manager, NULL, NULL, NULL, NULL, NULL, NULL, reader);
        CAM_LOGE("AImageReader listener registration failed");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }
    ANativeWindow *window = NULL;
    if (AImageReader_getWindow(reader, &window) != AMEDIA_OK || !window) {
        fv_camera_destroy_graph(manager, NULL, NULL, NULL, NULL, NULL, NULL, reader);
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }

    ACameraDevice_StateCallbacks device_callbacks = {
        .context = (void *)(uintptr_t)generation,
        .onDisconnected = fv_camera_disconnected,
        .onError = fv_camera_error,
    };
    ACameraDevice *device = NULL;
    if (ACameraManager_openCamera(manager, selected_id, &device_callbacks, &device) != ACAMERA_OK || !device) {
        fv_camera_destroy_graph(manager, NULL, NULL, NULL, NULL, NULL, NULL, reader);
        CAM_LOGE("Camera open failed; permission may be pending");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }

    ACaptureSessionOutputContainer *outputs = NULL;
    ACaptureSessionOutput *output = NULL;
    ACameraOutputTarget *target = NULL;
    ACaptureRequest *request = NULL;
    if (ACaptureSessionOutputContainer_create(&outputs) != ACAMERA_OK ||
        ACaptureSessionOutput_create(window, &output) != ACAMERA_OK ||
        ACaptureSessionOutputContainer_add(outputs, output) != ACAMERA_OK ||
        ACameraOutputTarget_create(window, &target) != ACAMERA_OK ||
        ACameraDevice_createCaptureRequest(device, TEMPLATE_PREVIEW, &request) != ACAMERA_OK ||
        ACaptureRequest_addTarget(request, target) != ACAMERA_OK) {
        fv_camera_destroy_graph(manager, device, NULL, request, target, output, outputs, reader);
        CAM_LOGE("Camera capture graph creation failed");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }

    ACameraCaptureSession_stateCallbacks session_callbacks = {
        .context = (void *)(uintptr_t)generation,
        .onClosed = fv_session_closed,
        .onReady = fv_session_ready,
        .onActive = fv_session_active,
    };
    ACameraCaptureSession *session = NULL;
    if (ACameraDevice_createCaptureSession(device, outputs, &session_callbacks, &session) != ACAMERA_OK || !session) {
        fv_camera_destroy_graph(manager, device, session, request, target, output, outputs, reader);
        CAM_LOGE("Camera capture session failed");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }

    pthread_mutex_lock(&s_camera.mutex);
    s_camera.manager = manager;
    s_camera.device = device;
    s_camera.session = session;
    s_camera.request = request;
    s_camera.target = target;
    s_camera.outputs = outputs;
    s_camera.output = output;
    s_camera.reader = reader;
    s_camera.reader_window = window;
    s_camera.active = false;
    s_camera.failed = false;
    pthread_mutex_unlock(&s_camera.mutex);

    if (ACameraCaptureSession_setRepeatingRequest(session, NULL, 1, &request, NULL) != ACAMERA_OK) {
        pthread_mutex_lock(&s_camera.mutex);
        s_camera.device = NULL;
        s_camera.session = NULL;
        s_camera.request = NULL;
        s_camera.target = NULL;
        s_camera.outputs = NULL;
        s_camera.output = NULL;
        s_camera.reader = NULL;
        s_camera.reader_window = NULL;
        s_camera.callback_reader = NULL;
        s_camera.failed = false;
        pthread_mutex_unlock(&s_camera.mutex);
        fv_camera_destroy_graph(manager, device, session, request, target, output, outputs, reader);
        CAM_LOGE("Camera repeating request failed");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }
    pthread_mutex_unlock(&s_camera_control_mutex);
    return true;
}

void fv_camera_stop(void) {
    pthread_mutex_lock(&s_camera_control_mutex);
    pthread_mutex_lock(&s_camera.mutex);
    ACameraCaptureSession *session = s_camera.session;
    ACameraDevice *device = s_camera.device;
    ACaptureRequest *request = s_camera.request;
    ACameraOutputTarget *target = s_camera.target;
    ACaptureSessionOutput *output = s_camera.output;
    ACaptureSessionOutputContainer *outputs = s_camera.outputs;
    AImageReader *reader = s_camera.reader;
    ACameraManager *manager = s_camera.manager;
    s_camera.session = NULL;
    s_camera.device = NULL;
    s_camera.request = NULL;
    s_camera.target = NULL;
    s_camera.output = NULL;
    s_camera.outputs = NULL;
    s_camera.reader = NULL;
    s_camera.reader_window = NULL;
    s_camera.callback_reader = NULL;
    s_camera.active = false;
    s_camera.failed = false;
    s_camera.frame_ready = false;
    s_camera.last_frame_ms = 0;
    s_camera.torch_on = false;
    pthread_mutex_unlock(&s_camera.mutex);
    fv_camera_destroy_graph(manager, device, session, request, target, output, outputs, reader);
    pthread_mutex_unlock(&s_camera_control_mutex);
}

bool fv_camera_copy_argb(uint32_t out_argb[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT]) {
    if (!out_argb) return false;
    pthread_mutex_lock(&s_camera.mutex);
    const bool ready = s_camera.frame_ready && s_camera.active && !s_camera.failed;
    if (ready) memcpy(out_argb, s_camera.argb, sizeof(s_camera.argb));
    pthread_mutex_unlock(&s_camera.mutex);
    return ready;
}

bool fv_camera_is_active(void) {
    pthread_mutex_lock(&s_camera.mutex);
    const bool active = s_camera.active;
    pthread_mutex_unlock(&s_camera.mutex);
    return active;
}

bool fv_camera_is_healthy(uint64_t max_age_ms) {
    pthread_mutex_lock(&s_camera.mutex);
    if (!s_camera.active || s_camera.failed) {
        pthread_mutex_unlock(&s_camera.mutex);
        return false;
    }
    const uint64_t now = current_camera_time_ms();
    const uint64_t elapsed = (now > s_camera.last_frame_ms) ? (now - s_camera.last_frame_ms) : 0;
    const bool healthy = elapsed <= max_age_ms;
    pthread_mutex_unlock(&s_camera.mutex);
    return healthy;
}

bool fv_camera_set_torch(bool enable) {
    pthread_mutex_lock(&s_camera_control_mutex);
    pthread_mutex_lock(&s_camera.mutex);
    if (!s_camera.session || !s_camera.request) {
        pthread_mutex_unlock(&s_camera.mutex);
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }
    const uint8_t ae_mode = ACAMERA_CONTROL_AE_MODE_ON;
    const uint8_t flash_mode = enable ? ACAMERA_FLASH_MODE_TORCH : ACAMERA_FLASH_MODE_OFF;
    camera_status_t status = ACaptureRequest_setEntry_u8(s_camera.request, ACAMERA_CONTROL_AE_MODE, 1, &ae_mode);
    if (status == ACAMERA_OK) {
        status = ACaptureRequest_setEntry_u8(s_camera.request, ACAMERA_FLASH_MODE, 1, &flash_mode);
    }
    if (status == ACAMERA_OK) {
        status = ACameraCaptureSession_setRepeatingRequest(s_camera.session, NULL, 1, &s_camera.request, NULL);
    }
    if (status == ACAMERA_OK) s_camera.torch_on = enable;
    pthread_mutex_unlock(&s_camera.mutex);
    pthread_mutex_unlock(&s_camera_control_mutex);
    return status == ACAMERA_OK;
}

bool fv_camera_is_torch_on(void) {
    pthread_mutex_lock(&s_camera.mutex);
    const bool on = s_camera.torch_on;
    pthread_mutex_unlock(&s_camera.mutex);
    return on;
}

int fv_camera_get_sensor_orientation(void) {
    pthread_mutex_lock(&s_camera.mutex);
    int ori = s_camera.sensor_orientation;
    pthread_mutex_unlock(&s_camera.mutex);
    return ori > 0 ? ori : 90;
}

int fv_android_get_display_rotation_degrees(ANativeActivity *activity) {
    if (!activity || !activity->vm) return 0;
    bool attached = false;
    JNIEnv *env = fv_get_env(activity, &attached);
    if (!env) return 0;

    jint rot_val = 0;
    jclass act_cls = (*env)->GetObjectClass(env, activity->clazz);
    if (act_cls) {
        jmethodID get_wm = (*env)->GetMethodID(env, act_cls, "getWindowManager", "()Landroid/view/WindowManager;");
        if (get_wm) {
            jobject wm = (*env)->CallObjectMethod(env, activity->clazz, get_wm);
            if (wm) {
                jclass wm_cls = (*env)->GetObjectClass(env, wm);
                jmethodID get_disp = wm_cls ? (*env)->GetMethodID(env, wm_cls, "getDefaultDisplay", "()Landroid/view/Display;") : NULL;
                if (get_disp) {
                    jobject disp = (*env)->CallObjectMethod(env, wm, get_disp);
                    if (disp) {
                        jclass disp_cls = (*env)->GetObjectClass(env, disp);
                        jmethodID get_rot = disp_cls ? (*env)->GetMethodID(env, disp_cls, "getRotation", "()I") : NULL;
                        if (get_rot) {
                            rot_val = (*env)->CallIntMethod(env, disp, get_rot);
                        }
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
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*activity->vm)->DetachCurrentThread(activity->vm);

    switch (rot_val) {
        case 1: return 90;
        case 2: return 180;
        case 3: return 270;
        default: return 0;
    }
}

int fv_camera_get_display_compensation_degrees(ANativeActivity *activity) {
    const int sensor_ori = fv_camera_get_sensor_orientation();
    const int disp_rot = fv_android_get_display_rotation_degrees(activity);
    return (sensor_ori - disp_rot + 360) % 360;
}
