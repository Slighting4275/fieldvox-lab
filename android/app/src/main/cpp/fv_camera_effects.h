#ifndef FV_CAMERA_EFFECTS_H
#define FV_CAMERA_EFFECTS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FV_CAM_FILTER_NORMAL, FV_CAM_FILTER_GREEN, FV_CAM_FILTER_PURPLE,
    FV_CAM_FILTER_DEPTH, FV_CAM_FILTER_HEAT, FV_CAM_FILTER_INVERTED,
    FV_CAM_FILTER_COUNT,
    FV_CAM_FILTER_OFF = FV_CAM_FILTER_COUNT /* Preview switch, outside the filter cycle. */
} FvCameraFilter;

const char *fv_camera_filter_name(FvCameraFilter filter);
FvCameraFilter fv_camera_filter_step(FvCameraFilter filter, int direction);
/* Separate input/output buffers, tightly packed 0xAARRGGBB. Rotation is clockwise.
 * Capacity is in pixels; rejected inputs leave the output and dimensions untouched. */
bool fv_camera_filter_frame(const uint32_t *src, int width, int height,
                            uint32_t *dst, size_t capacity, int rotation,
                            FvCameraFilter filter, bool torch, int *out_w, int *out_h);

enum { FV_CAMERA_GESTURE_SAMPLES = 16 };
typedef struct { float x; int64_t ns; } FvCameraGestureSample;
typedef struct {
    bool active;
    float start_x, start_y;
    unsigned count;
    FvCameraGestureSample samples[FV_CAMERA_GESTURE_SAMPLES];
} FvCameraGesture;
void fv_camera_gesture_begin(FvCameraGesture *gesture, float x, float y, int64_t ns, bool eligible);
void fv_camera_gesture_move(FvCameraGesture *gesture, float x, int64_t ns);
void fv_camera_gesture_cancel(FvCameraGesture *gesture);
/* +1 next (left swipe), -1 previous (right swipe), 0 no fling.
 * Eligibility is checked again at release to cover recording/overlay changes. */
int fv_camera_gesture_end(FvCameraGesture *gesture, float x, float y, int64_t ns, bool eligible);
#endif
