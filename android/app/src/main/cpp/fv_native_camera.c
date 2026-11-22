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
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
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
    FvCameraLensInfo lenses[8];
    size_t lens_count;
    size_t active_lens;
    atomic_uint desired_lens;
    bool lenses_scanned;
    bool front_facing;
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

static ACameraManager *ensure_manager_locked(void) {
    ACameraManager *manager = s_camera.manager;
    if (!manager) {
        manager = ACameraManager_create();
        if (manager) {
            pthread_mutex_lock(&s_camera.mutex);
            s_camera.manager = manager;
            pthread_mutex_unlock(&s_camera.mutex);
        }
    }
    return manager;
}

static bool read_lens(const char *id, const ACameraMetadata *metadata, FvCameraLensInfo *info) {
    ACameraMetadata_const_entry facing = {0}, focal = {0}, size = {0};
    if (strlen(id) >= sizeof(info->id) ||
        ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &facing) != ACAMERA_OK ||
        !facing.count || (facing.data.u8[0] != ACAMERA_LENS_FACING_BACK &&
                         facing.data.u8[0] != ACAMERA_LENS_FACING_FRONT)) return false;
    snprintf(info->id, sizeof(info->id), "%s", id);
    info->front_facing = facing.data.u8[0] == ACAMERA_LENS_FACING_FRONT;
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &focal) == ACAMERA_OK &&
        focal.count > 0) info->focal_mm = focal.data.f[0];
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &size) == ACAMERA_OK &&
        size.count > 0) info->sensor_width_mm = size.data.f[0];
    return true;
}

static size_t scan_physical_lenses(ACameraManager *manager, const ACameraMetadata *logical,
                                  const char *parent_id, FvCameraLensInfo *found, size_t cap) {
    size_t id_count = 0;
    const char *const *ids = NULL;
    if (!ACameraMetadata_isLogicalMultiCamera(logical, &id_count, &ids)) return 0;
    size_t count = 0;
    for (size_t i = 0; i < id_count; i++) {
        ACameraMetadata *metadata = NULL;
        if (ACameraManager_getCameraCharacteristics(manager, ids[i], &metadata) != ACAMERA_OK || !metadata)
            continue;
        FvCameraLensInfo lens = {0};
        const bool usable = read_lens(ids[i], metadata, &lens) && !lens.front_facing &&
                            isfinite(lens.focal_mm) && lens.focal_mm > 0.0f;
        ACameraMetadata_free(metadata);
        if (!usable) continue;
        snprintf(lens.parent_id, sizeof(lens.parent_id), "%s", parent_id);
        size_t index = 0;
        while (index < count && fabsf(found[index].focal_mm - lens.focal_mm) > 0.05f) index++;
        /* Prefer the uncropped sensor when the HAL exposes multiple modes for one lens. */
        if (index < count) {
            if (lens.sensor_width_mm > found[index].sensor_width_mm) found[index] = lens;
        } else if (count < cap) {
            found[count++] = lens;
        }
    }
    return count;
}

static void scan_lenses_locked(ACameraManager *manager) {
    pthread_mutex_lock(&s_camera.mutex);
    const bool already = s_camera.lenses_scanned;
    pthread_mutex_unlock(&s_camera.mutex);
    if (already || !manager) return;

    ACameraIdList *ids = NULL;
    if (ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK || !ids || ids->numCameras < 1) {
        if (ids) ACameraManager_deleteCameraIdList(ids);
        return;
    }

    FvCameraLensInfo found[sizeof(s_camera.lenses) / sizeof(s_camera.lenses[0])] = {0};
    const size_t capacity = sizeof(found) / sizeof(found[0]);
    size_t count = 0u;
    for (int i = 0; i < ids->numCameras && count < capacity; i++) {
        ACameraMetadata *metadata = NULL;
        if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &metadata) != ACAMERA_OK || !metadata) {
            continue;
        }
        if (read_lens(ids->cameraIds[i], metadata, &found[count])) {
            count++;
            const size_t remaining_public = (size_t)(ids->numCameras - i - 1);
            if (!found[count - 1].front_facing && capacity - count > remaining_public) {
                count += scan_physical_lenses(manager, metadata, found[count - 1].id, found + count,
                                              capacity - count - remaining_public);
            }
        }
        ACameraMetadata_free(metadata);
    }
    ACameraManager_deleteCameraIdList(ids);
    fv_camera_build_lens_labels(found, count);

    pthread_mutex_lock(&s_camera.mutex);
    memcpy(s_camera.lenses, found, sizeof(found));
    s_camera.lens_count = count;
    s_camera.lenses_scanned = true;
    if (atomic_load(&s_camera.desired_lens) >= count) atomic_store(&s_camera.desired_lens, 0u);
    pthread_mutex_unlock(&s_camera.mutex);
}

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

    ACameraManager *manager = ensure_manager_locked();
    scan_lenses_locked(manager);

    FvCameraLensInfo selected = {0};
    size_t lens_idx = 0u;
    bool front_facing = false;
    int detected_orientation = 90;

    pthread_mutex_lock(&s_camera.mutex);
    if (s_camera.lens_count > 0u) {
        lens_idx = (size_t)(atomic_load(&s_camera.desired_lens) % s_camera.lens_count);
        selected = s_camera.lenses[lens_idx];
        front_facing = selected.front_facing;
    }
    pthread_mutex_unlock(&s_camera.mutex);

    if (!manager || !selected.id[0]) {
        CAM_LOGE("No native camera is available");
        pthread_mutex_unlock(&s_camera_control_mutex);
        return false;
    }
    ACameraMetadata *metadata = NULL;
    if (ACameraManager_getCameraCharacteristics(manager, selected.id, &metadata) == ACAMERA_OK && metadata) {
        ACameraMetadata_const_entry ori_entry = {0};
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_ORIENTATION, &ori_entry) == ACAMERA_OK &&
            ori_entry.count > 0) {
            detected_orientation = ori_entry.data.i32[0];
        }
        ACameraMetadata_free(metadata);
    }
    pthread_mutex_lock(&s_camera.mutex);
    s_camera.sensor_orientation = detected_orientation;
    s_camera.front_facing = front_facing;
    s_camera.active_lens = lens_idx;
    pthread_mutex_unlock(&s_camera.mutex);
    const bool physical = selected.parent_id[0] != '\0';
    const char *device_id = physical ? selected.parent_id : selected.id;
    CAM_LOGI("Selected camera '%s' from device '%s' (%s, orientation: %d deg)",
             selected.id, device_id, selected.label, detected_orientation);

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
    if (ACameraManager_openCamera(manager, device_id, &device_callbacks, &device) != ACAMERA_OK || !device) {
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
        (physical ? ACaptureSessionPhysicalOutput_create(window, selected.id, &output)
                  : ACaptureSessionOutput_create(window, &output)) != ACAMERA_OK ||
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

bool fv_camera_copy_argb(uint32_t out_argb[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT],
                        int display_rotation_deg, int *rotation_deg, bool *front_facing,
                        bool *torch_on) {
    if (!out_argb) return false;
    pthread_mutex_lock(&s_camera.mutex);
    const bool ready = s_camera.frame_ready && s_camera.active && !s_camera.failed;
    if (ready) {
        memcpy(out_argb, s_camera.argb, sizeof(s_camera.argb));
        *rotation_deg = fv_camera_compensation_degrees_facing(s_camera.sensor_orientation,
                                                              display_rotation_deg,
                                                              s_camera.front_facing);
        *front_facing = s_camera.front_facing;
        *torch_on = s_camera.torch_on;
    }
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

static size_t camera_lens_count(void) {
    pthread_mutex_lock(&s_camera_control_mutex);
    scan_lenses_locked(ensure_manager_locked());
    pthread_mutex_lock(&s_camera.mutex);
    const size_t n = s_camera.lens_count;
    pthread_mutex_unlock(&s_camera.mutex);
    pthread_mutex_unlock(&s_camera_control_mutex);
    return n;
}

size_t fv_camera_current_lens(void) {
    return (size_t)atomic_load(&s_camera.desired_lens);
}

void fv_camera_copy_lens_label(size_t index, char *out, size_t cap) {
    if (!out || !cap) return;
    pthread_mutex_lock(&s_camera.mutex);
    const char *lbl = (index < s_camera.lens_count && s_camera.lenses[index].label[0])
                          ? s_camera.lenses[index].label
                          : "BACK";
    snprintf(out, cap, "%s", lbl);
    pthread_mutex_unlock(&s_camera.mutex);
}

bool fv_camera_cycle_lens(void) {
    const size_t n = camera_lens_count();
    if (n <= 1u) return false;
    const unsigned next = (unsigned)(((size_t)atomic_load(&s_camera.desired_lens) + 1u) % n);
    atomic_store(&s_camera.desired_lens, next);
    return true;
}

bool fv_camera_is_front_facing(void) {
    pthread_mutex_lock(&s_camera.mutex);
    const bool front = s_camera.front_facing;
    pthread_mutex_unlock(&s_camera.mutex);
    return front;
}

bool fv_camera_needs_reopen(void) {
    pthread_mutex_lock(&s_camera.mutex);
    const bool need = s_camera.lens_count > 0u &&
                      s_camera.active_lens != (size_t)(atomic_load(&s_camera.desired_lens) % s_camera.lens_count);
    pthread_mutex_unlock(&s_camera.mutex);
    return need;
}
