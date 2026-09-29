#include "fv_native_camera.h"

#include <android/log.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadataTags.h>
#include <jni.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define FV_CAM_TAG "FieldVoxCamera"
#define CAM_LOGI(...) __android_log_print(ANDROID_LOG_INFO, FV_CAM_TAG, __VA_ARGS__)
#define CAM_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, FV_CAM_TAG, __VA_ARGS__)

typedef struct {
    pthread_mutex_t mutex;
    ACameraManager *manager;
    ACameraDevice *device;
    ACameraCaptureSession *session;
    ACaptureRequest *request;
    ACameraOutputTarget *target;
    ACaptureSessionOutputContainer *outputs;
    ACaptureSessionOutput *output;
    AImageReader *reader;
    ANativeWindow *reader_window;
    uint8_t luma[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT];
    bool frame_ready;
    bool active;
    bool failed;
} FvCameraState;

static FvCameraState s_camera = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};
static pthread_mutex_t s_camera_control_mutex = PTHREAD_MUTEX_INITIALIZER;

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

void fv_android_request_permission(ANativeActivity *activity, const char *permission, int request_code) {
    bool attached = false;
    JNIEnv *env = fv_get_env(activity, &attached);
    if (!env || !permission) return;
    jclass cls = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID request = cls ? (*env)->GetMethodID(env, cls, "requestPermissions", "([Ljava/lang/String;I)V") : NULL;
    jclass string_cls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray values = string_cls ? (*env)->NewObjectArray(env, 1, string_cls, NULL) : NULL;
    if (request && values) {
        jstring value = (*env)->NewStringUTF(env, permission);
        (*env)->SetObjectArrayElement(env, values, 0, value);
        (*env)->CallVoidMethod(env, activity->clazz, request, values, request_code);
        if (value) (*env)->DeleteLocalRef(env, value);
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

void fv_android_request_runtime_permissions(ANativeActivity *activity) {
    fv_android_request_permission(activity, "android.permission.RECORD_AUDIO", 60);
    fv_android_request_permission(activity, "android.permission.CAMERA", 61);
}

static void fv_camera_image_available(void *context, AImageReader *reader) {
    (void)context;
    AImage *image = NULL;
    if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) return;
    uint8_t *data = NULL;
    int data_len = 0;
    int32_t row_stride = 0;
    int32_t pixel_stride = 1;
    if (AImage_getPlaneData(image, 0, &data, &data_len) == AMEDIA_OK && data &&
        AImage_getPlaneRowStride(image, 0, &row_stride) == AMEDIA_OK) {
        AImage_getPlanePixelStride(image, 0, &pixel_stride);
        pthread_mutex_lock(&s_camera.mutex);
        for (int y = 0; y < FV_CAMERA_HEIGHT; y++) {
            const uint8_t *src = data + y * row_stride;
            uint8_t *dst = s_camera.luma + y * FV_CAMERA_WIDTH;
            for (int x = 0; x < FV_CAMERA_WIDTH; x++) dst[x] = src[x * pixel_stride];
        }
        s_camera.frame_ready = true;
        pthread_mutex_unlock(&s_camera.mutex);
    }
    AImage_delete(image);
}

static void fv_camera_disconnected(void *context, ACameraDevice *device) {
    (void)context;
    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.device == device) {
        s_camera.active = false;
        s_camera.failed = true;
    }
    pthread_mutex_unlock(&s_camera.mutex);
    CAM_LOGE("Camera disconnected");
}

static void fv_camera_error(void *context, ACameraDevice *device, int error) {
    (void)context;
    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.device == device) {
        s_camera.active = false;
        s_camera.failed = true;
    }
    pthread_mutex_unlock(&s_camera.mutex);
    CAM_LOGE("Camera error: %d", error);
}

static void fv_session_closed(void *context, ACameraCaptureSession *session) {
    (void)context;
    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.session == session) {
        s_camera.active = false;
        s_camera.failed = true;
    }
    pthread_mutex_unlock(&s_camera.mutex);
}

static void fv_session_ready(void *context, ACameraCaptureSession *session) {
    (void)context;
    (void)session;
}

static void fv_session_active(void *context, ACameraCaptureSession *session) {
    (void)context;
    pthread_mutex_lock(&s_camera.mutex);
    const bool current_session = s_camera.session == session;
    if (current_session) s_camera.active = true;
    pthread_mutex_unlock(&s_camera.mutex);
    if (current_session) CAM_LOGI("Native camera preview active");
}

bool fv_camera_start(void) {
    pthread_mutex_lock(&s_camera_control_mutex);
    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.device || s_camera.manager) {
        const bool usable_or_starting = !s_camera.failed;
        pthread_mutex_unlock(&s_camera.mutex);
        pthread_mutex_unlock(&s_camera_control_mutex);
        return usable_or_starting;
    }
    pthread_mutex_unlock(&s_camera.mutex);

    ACameraManager *manager = ACameraManager_create();
    ACameraIdList *ids = NULL;
    if (!manager || ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK || !ids || ids->numCameras < 1) {
        if (ids) ACameraManager_deleteCameraIdList(ids);
        if (manager) ACameraManager_delete(manager);
        CAM_LOGE("No native camera is available");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }

    const char *selected = ids->cameraIds[0];
    for (int i = 0; i < ids->numCameras; i++) {
        ACameraMetadata *metadata = NULL;
        ACameraMetadata_const_entry entry;
        if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &metadata) == ACAMERA_OK && metadata &&
            ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK &&
            entry.count > 0 && entry.data.u8[0] == ACAMERA_LENS_FACING_BACK) {
            selected = ids->cameraIds[i];
            ACameraMetadata_free(metadata);
            break;
        }
        if (metadata) ACameraMetadata_free(metadata);
    }
    char selected_id[64];
    snprintf(selected_id, sizeof(selected_id), "%s", selected);
    ACameraManager_deleteCameraIdList(ids);

    AImageReader *reader = NULL;
    if (AImageReader_new(FV_CAMERA_WIDTH, FV_CAMERA_HEIGHT, AIMAGE_FORMAT_YUV_420_888, 3, &reader) != AMEDIA_OK || !reader) {
        ACameraManager_delete(manager);
        CAM_LOGE("AImageReader creation failed");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }
    AImageReader_ImageListener listener = { .context = NULL, .onImageAvailable = fv_camera_image_available };
    AImageReader_setImageListener(reader, &listener);
    ANativeWindow *window = NULL;
    if (AImageReader_getWindow(reader, &window) != AMEDIA_OK || !window) {
        AImageReader_delete(reader);
        ACameraManager_delete(manager);
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }

    ACameraDevice_StateCallbacks device_callbacks = {
        .context = NULL,
        .onDisconnected = fv_camera_disconnected,
        .onError = fv_camera_error,
    };
    ACameraDevice *device = NULL;
    if (ACameraManager_openCamera(manager, selected_id, &device_callbacks, &device) != ACAMERA_OK || !device) {
        AImageReader_delete(reader);
        ACameraManager_delete(manager);
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
        if (request) ACaptureRequest_free(request);
        if (target) ACameraOutputTarget_free(target);
        if (output) ACaptureSessionOutput_free(output);
        if (outputs) ACaptureSessionOutputContainer_free(outputs);
        ACameraDevice_close(device);
        AImageReader_delete(reader);
        ACameraManager_delete(manager);
        CAM_LOGE("Camera capture graph creation failed");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }

    ACameraCaptureSession_stateCallbacks session_callbacks = {
        .context = NULL,
        .onClosed = fv_session_closed,
        .onReady = fv_session_ready,
        .onActive = fv_session_active,
    };
    ACameraCaptureSession *session = NULL;
    if (ACameraDevice_createCaptureSession(device, outputs, &session_callbacks, &session) != ACAMERA_OK || !session) {
        if (session) ACameraCaptureSession_close(session);
        ACaptureRequest_free(request);
        ACameraOutputTarget_free(target);
        ACaptureSessionOutput_free(output);
        ACaptureSessionOutputContainer_free(outputs);
        ACameraDevice_close(device);
        AImageReader_delete(reader);
        ACameraManager_delete(manager);
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
        s_camera.manager = NULL;
        s_camera.device = NULL;
        s_camera.session = NULL;
        s_camera.request = NULL;
        s_camera.target = NULL;
        s_camera.outputs = NULL;
        s_camera.output = NULL;
        s_camera.reader = NULL;
        s_camera.reader_window = NULL;
        s_camera.failed = false;
        pthread_mutex_unlock(&s_camera.mutex);
        ACameraCaptureSession_close(session);
        ACaptureRequest_free(request);
        ACameraOutputTarget_free(target);
        ACaptureSessionOutput_free(output);
        ACaptureSessionOutputContainer_free(outputs);
        ACameraDevice_close(device);
        AImageReader_delete(reader);
        ACameraManager_delete(manager);
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
    s_camera.active = false;
    s_camera.failed = false;
    s_camera.frame_ready = false;
    pthread_mutex_unlock(&s_camera.mutex);
    if (session) {
        ACameraCaptureSession_stopRepeating(session);
        ACameraCaptureSession_close(session);
    }
    if (request) ACaptureRequest_free(request);
    if (target) ACameraOutputTarget_free(target);
    if (output) ACaptureSessionOutput_free(output);
    if (outputs) ACaptureSessionOutputContainer_free(outputs);
    if (device) ACameraDevice_close(device);
    if (reader) AImageReader_delete(reader);
    if (manager) ACameraManager_delete(manager);
    pthread_mutex_unlock(&s_camera_control_mutex);
}

bool fv_camera_copy_luma(uint8_t out_luma[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT]) {
    if (!out_luma) return false;
    pthread_mutex_lock(&s_camera.mutex);
    const bool ready = s_camera.frame_ready;
    if (ready) memcpy(out_luma, s_camera.luma, sizeof(s_camera.luma));
    pthread_mutex_unlock(&s_camera.mutex);
    return ready;
}

bool fv_camera_is_active(void) {
    pthread_mutex_lock(&s_camera.mutex);
    const bool active = s_camera.active;
    pthread_mutex_unlock(&s_camera.mutex);
    return active;
}
