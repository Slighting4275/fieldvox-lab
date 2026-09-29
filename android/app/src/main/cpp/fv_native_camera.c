#include "fv_native_camera.h"

#include <android/log.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadataTags.h>
#include <jni.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <math.h>
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
    int sensor_orientation;
} FvCameraState;

static FvCameraState s_camera = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .sensor_orientation = 90,
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

static void fv_camera_image_available(void *context, AImageReader *reader) {
    (void)context;
    AImage *image = NULL;
    if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) return;
    uint8_t *data = NULL;
    int data_len = 0;
    int32_t row_stride = 0;
    int32_t pixel_stride = 1;
    if (AImage_getPlaneData(image, 0, &data, &data_len) == AMEDIA_OK && data &&
        AImage_getPlaneRowStride(image, 0, &row_stride) == AMEDIA_OK &&
        row_stride >= FV_CAMERA_WIDTH) {
        AImage_getPlanePixelStride(image, 0, &pixel_stride);
        static int s_logged = 0;
        if (s_logged++ < 3) {
            CAM_LOGI("image plane 0: data_len=%d, row_stride=%d, pixel_stride=%d, needed=%zu",
                     data_len, row_stride, pixel_stride,
                     (size_t)(FV_CAMERA_HEIGHT - 1) * (size_t)row_stride + (size_t)FV_CAMERA_WIDTH * (size_t)pixel_stride);
        }
        if (pixel_stride > 0 &&
            data_len >= (int)((size_t)(FV_CAMERA_HEIGHT - 1) * (size_t)row_stride + (size_t)FV_CAMERA_WIDTH * (size_t)pixel_stride)) {
            pthread_mutex_lock(&s_camera.mutex);
            for (int y = 0; y < FV_CAMERA_HEIGHT; y++) {
                const uint8_t *src = data + (size_t)y * (size_t)row_stride;
                uint8_t *dst = s_camera.luma + y * FV_CAMERA_WIDTH;
                for (int x = 0; x < FV_CAMERA_WIDTH; x++) dst[x] = src[x * pixel_stride];
            }
            s_camera.frame_ready = true;
            pthread_mutex_unlock(&s_camera.mutex);
        }
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
    if (reader) {
        AImageReader_setImageListener(reader, NULL);
        AImageReader_delete(reader);
    }
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

const char *fv_camera_filter_name(FvCameraFilter filter) {
    switch (filter) {
        case FV_CAM_FILTER_PHOSPHOR: return "PHOSPHOR";
        case FV_CAM_FILTER_THERMAL:  return "THERMAL";
        case FV_CAM_FILTER_CYAN:     return "CYAN";
        case FV_CAM_FILTER_OFF:      return "STEALTH";
        default:                     return "OFF";
    }
}

static inline uint32_t clamp_u32(int32_t val, int32_t min_val, int32_t max_val) {
    if (val < min_val) return (uint32_t)min_val;
    if (val > max_val) return (uint32_t)max_val;
    return (uint32_t)val;
}

void fv_camera_luma_to_argb(const uint8_t *luma, uint32_t *out_argb, int count, FvCameraFilter filter) {
    if (!luma || !out_argb || count <= 0) return;

    if (filter == FV_CAM_FILTER_OFF) {
        memset(out_argb, 0, (size_t)count * sizeof(uint32_t));
        return;
    }

    for (int i = 0; i < count; i++) {
        const uint32_t l = (uint32_t)luma[i];
        uint32_t r = 0, g = 0, b = 0;

        switch (filter) {
            case FV_CAM_FILTER_PHOSPHOR:
                r = l / 10u;
                g = l;
                b = l / 5u;
                break;
            case FV_CAM_FILTER_THERMAL:
                r = clamp_u32((int32_t)(l * 2u), 0, 255);
                g = (l > 80u) ? clamp_u32((int32_t)((l - 80u) * 2u), 0, 255) : 0u;
                b = (l > 170u) ? clamp_u32((int32_t)((l - 170u) * 3u), 0, 255) : (l / 16u);
                break;
            case FV_CAM_FILTER_CYAN:
                r = l / 12u;
                g = clamp_u32((int32_t)(l + (l / 6u)), 0, 255);
                b = l;
                break;
            case FV_CAM_FILTER_OFF:
            default:
                r = g = b = 0;
                break;
        }

        out_argb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
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

void fv_camera_luma_to_argb_rotated(const uint8_t *luma, int src_w, int src_h,
                                   uint32_t *out_argb, int *out_w, int *out_h,
                                   int rot_deg, FvCameraFilter filter) {
    if (!luma || !out_argb || src_w <= 0 || src_h <= 0) return;

    rot_deg = (rot_deg % 360 + 360) % 360;
    const bool swap_dims = (rot_deg == 90 || rot_deg == 270);
    const int dw = swap_dims ? src_h : src_w;
    const int dh = swap_dims ? src_w : src_h;

    if (out_w) *out_w = dw;
    if (out_h) *out_h = dh;

    if (filter == FV_CAM_FILTER_OFF) {
        memset(out_argb, 0, (size_t)dw * (size_t)dh * sizeof(uint32_t));
        return;
    }

    /* Dynamic range AGC for paranormal night-vision: boosts low-light shadows up to 5x */
    uint32_t max_l = 10;
    const int total_pixels = src_w * src_h;
    for (int i = 0; i < total_pixels; i += 16) {
        if (luma[i] > max_l) max_l = luma[i];
    }
    const float gain = fminf(5.0f, 220.0f / (float)max_l);

    for (int sy = 0; sy < src_h; sy++) {
        const uint8_t *luma_row = luma + sy * src_w;
        for (int sx = 0; sx < src_w; sx++) {
            const uint32_t l = clamp_u32((int32_t)((float)luma_row[sx] * gain), 0, 255);
            uint32_t r = 0, g = 0, b = 0;

            switch (filter) {
                case FV_CAM_FILTER_PHOSPHOR:
                    r = l / 10u;
                    g = l;
                    b = l / 5u;
                    break;
                case FV_CAM_FILTER_THERMAL:
                    r = clamp_u32((int32_t)(l * 2u), 0, 255);
                    g = (l > 80u) ? clamp_u32((int32_t)((l - 80u) * 2u), 0, 255) : 0u;
                    b = (l > 170u) ? clamp_u32((int32_t)((l - 170u) * 3u), 0, 255) : (l / 16u);
                    break;
                case FV_CAM_FILTER_CYAN:
                    r = l / 12u;
                    g = clamp_u32((int32_t)(l + (l / 6u)), 0, 255);
                    b = l;
                    break;
                case FV_CAM_FILTER_OFF:
                default:
                    r = g = b = 0;
                    break;
            }

            const uint32_t color = 0xFF000000u | (r << 16) | (g << 8) | b;

            int dx, dy;
            switch (rot_deg) {
                case 90:
                    dx = (src_h - 1) - sy;
                    dy = sx;
                    break;
                case 180:
                    dx = (src_w - 1) - sx;
                    dy = (src_h - 1) - sy;
                    break;
                case 270:
                    dx = sy;
                    dy = (src_w - 1) - sx;
                    break;
                case 0:
                default:
                    dx = sx;
                    dy = sy;
                    break;
            }

            out_argb[dy * dw + dx] = color;
        }
    }
}

