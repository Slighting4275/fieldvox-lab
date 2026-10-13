#ifndef FIELDVOX_NATIVE_RADIO_H
#define FIELDVOX_NATIVE_RADIO_H
#include <jni.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fv_sensor_dsp.h"
#define FV_RADIO_DECKS 10
/* One process-wide, bounded network worker. No continuous background streams. */
bool fv_radio_start(JavaVM *vm,const char *app_directory,const char *language,int station_count);
void fv_radio_set_enabled(bool enabled);
int fv_radio_selected_station(int channel);
bool fv_radio_request(int channel,const FvVoxDecimalTrigger *trigger);
/* Transfers PCM ownership to the audio thread, including a NULL result on failure. */
bool fv_radio_take_result(int16_t **pcm,size_t *frames,int *station,FvVoxDecimalTrigger *trigger);
bool fv_radio_busy(void);
#endif
