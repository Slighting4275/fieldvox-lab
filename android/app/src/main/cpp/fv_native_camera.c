/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_native_camera.h"
#include "fv_media_decode.h"

#include <android/log.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadataTags.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
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

    pthread_mutex_lock(&s_camera.mutex);
    if (fv_media_image_to_argb(latest_image, s_camera.argb, FV_CAMERA_WIDTH, FV_CAMERA_HEIGHT)) {
        s_camera.frame_ready = true;
        s_camera.last_frame_ms = current_camera_time_ms();
    }
    pthread_mutex_unlock(&s_camera.mutex);
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

int fv_camera_get_compensation_for_display(int display_rotation_deg) {
    return fv_camera_compensation_degrees(fv_camera_get_sensor_orientation(), display_rotation_deg);
}
