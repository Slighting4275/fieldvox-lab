#ifndef FIELDVOX_NATIVE_CAMERA_H
#define FIELDVOX_NATIVE_CAMERA_H

#include <android/native_activity.h>
#include <stdbool.h>
#include <stdint.h>

#define FV_CAMERA_WIDTH 640
#define FV_CAMERA_HEIGHT 360

typedef enum {
    FV_CAM_FILTER_PHOSPHOR = 0, /* Night-vision green */
    FV_CAM_FILTER_THERMAL = 1,  /* Infrared heat-map red/amber */
    FV_CAM_FILTER_CYAN = 2,     /* Twilight spectral cyan */
    FV_CAM_FILTER_OFF = 3,      /* OLED stealth black */
    FV_CAM_FILTER_COUNT = 4
} FvCameraFilter;

void fv_android_request_runtime_permissions(ANativeActivity *activity);
void fv_android_request_permissions(ANativeActivity *activity, const char * const *permissions, int count, int request_code);
void fv_android_request_permission(ANativeActivity *activity, const char *permission, int request_code);
bool fv_android_has_permission(ANativeActivity *activity, const char *permission);
bool fv_camera_start(void);
void fv_camera_stop(void);
bool fv_camera_copy_luma(uint8_t out_luma[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT]);
bool fv_camera_is_active(void);
bool fv_camera_is_healthy(uint64_t max_age_ms);
const char *fv_camera_filter_name(FvCameraFilter filter);
void fv_camera_luma_to_argb(const uint8_t *luma, uint32_t *out_argb, int count, FvCameraFilter filter);
void fv_camera_luma_to_argb_rotated(const uint8_t *luma, int src_w, int src_h,
                                   uint32_t *out_argb, int *out_w, int *out_h,
                                   int rot_deg, FvCameraFilter filter);
int fv_camera_get_sensor_orientation(void);
int fv_android_get_display_rotation_degrees(ANativeActivity *activity);
int fv_camera_get_display_compensation_degrees(ANativeActivity *activity);
bool fv_camera_set_torch(bool enable);
bool fv_camera_is_torch_on(void);
void fv_android_vibrate(ANativeActivity *activity, int ms);
void fv_android_vibrate_double(ANativeActivity *activity);
void fv_android_open_file(ANativeActivity *activity, const char *path, const char *mime_type);

#endif
