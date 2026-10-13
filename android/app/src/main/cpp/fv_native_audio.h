#ifndef FIELDVOX_NATIVE_AUDIO_H
#define FIELDVOX_NATIVE_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define FV_NATIVE_SPECTRUM_BINS 64

void fv_audio_dsp_reset(void);
void fv_audio_capture_and_analyze(const int16_t *samples, int count);
void fv_audio_apply_echo(int16_t *samples, int count);
void fv_audio_apply_echo_ex(int16_t *samples, int count, int delay_ms, float mix);
void fv_audio_apply_noise(int16_t *samples, int count, int amplitude);
void fv_audio_reverse(int16_t *samples, int count);
void fv_audio_get_spectrum(float out_bins[FV_NATIVE_SPECTRUM_BINS]);
bool fv_audio_export_preroll_wav(const char *path, int pre_seconds);

void fv_vocal_bandpass(int16_t *samples, int count, uint32_t sample_rate);
void fv_apply_schroeder_reverb(int16_t *samples, int count);
void fv_apply_schroeder_reverb_ex(int16_t *samples, int count, float mix);
void fv_apply_granular_pitch(int16_t *samples, int count, int semitones);

#define FV_VOX_MAX_VOICES 10

typedef struct {
    bool active;
    uint64_t start_time_ms;
    uint64_t play_duration_ms;
    uint64_t release_time_ms;
    int station_id;
    float speed;
    float volume;
    int pitch_semitones;
    int tap_index;
} FvVoxVoiceSlot;

typedef struct {
    FvVoxVoiceSlot voices[FV_VOX_MAX_VOICES];
    float reverb_mix;
} FvVoxVoicePool;

/*
 * 10-voice pool with speed-compensated play duration and reverb-delayed voice release:
 * release_time = start_time + (grain_duration / speed) + (9000 ms * reverb_mix).
 */
void fv_vox_voice_pool_init(FvVoxVoicePool *pool, float reverb_mix);
void fv_vox_voice_pool_set_reverb(FvVoxVoicePool *pool, float reverb_mix);
int fv_vox_voice_pool_allocate(FvVoxVoicePool *pool, uint64_t now_ms, uint64_t grain_duration_ms,
                               float playback_speed, int station_id);
int fv_vox_voice_pool_allocate_ex(FvVoxVoicePool *pool, uint64_t now_ms, uint64_t grain_duration_ms,
                                  float playback_speed, int station_id, float volume,
                                  int pitch_semitones, int tap_index);
void fv_vox_voice_pool_update(FvVoxVoicePool *pool, uint64_t now_ms);
bool fv_vox_voice_is_playing(const FvVoxVoicePool *pool, int voice_idx, uint64_t now_ms);
bool fv_vox_voice_is_busy(const FvVoxVoicePool *pool, int voice_idx, uint64_t now_ms);

/*
 * Sweep-rate inter-carrier static burst synthesis:
 * [-0.2, +0.2] * 32767.0f, default volume 0.02f (-34 dB).
 * Cycle period: (sweep_rate_ms + 40 ms).
 */
void fv_audio_generate_sweep_static(int16_t *samples, int count, uint32_t *rng, float volume);
bool fv_audio_is_sweep_burst_active(uint64_t clock_ms, uint32_t sweep_rate_ms);

#endif
