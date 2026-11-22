.RECIPEPREFIX := >

CC ?= cc
CFLAGS ?= -std=c23 -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
LDFLAGS ?= -lm

MAIN_BIN := fieldvox
DUMP_BIN := fv-blobdump
CSV_BIN := fv-csvvox
TEST_BIN := fv-unit-test
TYPOGRAPHY_FONT ?= $(shell fc-match -f "%{file}" sans 2>/dev/null)

MAIN_SRC := src/main.c src/fv_core.c ../android/app/src/main/cpp/fv_recording_data.c ../android/app/src/main/cpp/fv_sensor_dsp.c
DUMP_SRC := src/fv_blobdump.c src/fv_core.c ../android/app/src/main/cpp/fv_recording_data.c ../android/app/src/main/cpp/fv_sensor_dsp.c
CSV_SRC := src/fv_csvvox.c src/fv_core.c ../android/app/src/main/cpp/fv_recording_data.c ../android/app/src/main/cpp/fv_sensor_dsp.c
TEST_SRC := tests/test_native_units.c ../android/app/src/main/cpp/fv_stage1r_native_audio.c ../android/app/src/main/cpp/fv_hit_capture.c ../android/app/src/main/cpp/fv_vox_controls.c ../android/app/src/main/cpp/fv_sensor_dsp.c ../android/app/src/main/cpp/fv_local_bank.c ../android/app/src/main/cpp/fv_recording_data.c ../android/app/src/main/cpp/fv_ui_layout.c ../android/app/src/main/cpp/fv_camera_effects.c ../android/app/src/main/cpp/fv_fragment_engine.c ../android/app/src/main/cpp/fv_environment_reverb.c .audio-build/LVREV_ApplyNewSettings.o .audio-build/LVREV_ClearAudioBuffers.o .audio-build/LVREV_GetControlParameters.o .audio-build/LVREV_GetInstanceHandle.o .audio-build/LVREV_GetMemoryTable.o .audio-build/LVREV_Process.o .audio-build/LVREV_SetControlParameters.o .audio-build/LVREV_Tables.o .audio-build/Add2_Sat_32x32.o .audio-build/Copy_16.o .audio-build/Core_MixHard_2St_D32C31_SAT.o .audio-build/Core_MixInSoft_D32C31_SAT.o .audio-build/Core_MixSoft_1St_D32C31_WRA.o .audio-build/FO_1I_D32F32C31_TRC_WRA_01.o .audio-build/FO_1I_D32F32Cll_TRC_WRA_01_Init.o .audio-build/From2iToMono_32.o .audio-build/InstAlloc.o .audio-build/JoinTo2i_32x32.o .audio-build/LoadConst_32.o .audio-build/LVM_FO_HPF.o .audio-build/LVM_FO_LPF.o .audio-build/LVM_GetOmega.o .audio-build/LVM_Mixer_TimeConstant.o .audio-build/LVM_Polynomial.o .audio-build/LVM_Power10.o .audio-build/Mac3s_Sat_32x16.o .audio-build/MixInSoft_D32C31_SAT.o .audio-build/MixSoft_1St_D32C31_WRA.o .audio-build/MixSoft_2St_D32C31_SAT.o .audio-build/MonoTo2I_32.o .audio-build/Mult3s_32x16.o .audio-build/Shift_Sat_v32xv32.o .audio-build/sonic.o
HDR := src/fv_core.h

.PHONY: all clean run inspect dump extract csvtest audition test blobgen

all: $(MAIN_BIN) $(DUMP_BIN) $(CSV_BIN) $(TEST_BIN) fv-recording-test fv-fragment-test fv-typography-test fv-camera-test

$(MAIN_BIN): $(MAIN_SRC) $(HDR)
>$(CC) $(CFLAGS) -o $@ $(MAIN_SRC) $(LDFLAGS)

$(DUMP_BIN): $(DUMP_SRC) $(HDR)
>$(CC) $(CFLAGS) -o $@ $(DUMP_SRC) $(LDFLAGS)

$(CSV_BIN): $(CSV_SRC) $(HDR)
>$(CC) $(CFLAGS) -o $@ $(CSV_SRC) $(LDFLAGS)

$(TEST_BIN): $(TEST_SRC) $(ALGORITHM_OBJ) $(wildcard ../android/app/src/main/cpp/*.h)
>$(CC) $(CFLAGS) $(ALGORITHM_FLAGS) -o $@ $(TEST_SRC) $(LDFLAGS) -pthread

fv-recording-test: tests/test_recordings.c ../android/app/src/main/cpp/fv_recording_data.c ../android/app/src/main/cpp/fv_recording_data.h
>$(CC) $(CFLAGS) -o $@ tests/test_recordings.c ../android/app/src/main/cpp/fv_recording_data.c $(LDFLAGS)

run: all
>./$(MAIN_BIN) --seconds 24 --threshold 1.80 --seed 0xBEEFBEEF

inspect:
>ls -lh out
>sha256sum out/* || true
>echo
>echo "=== first 12 JSONL log lines ==="
>head -n 12 out/session.jsonl || true

dump: all
>./$(DUMP_BIN) --input out/session.fvblobs --summary

extract: all
>mkdir -p out/extracted
>./$(DUMP_BIN) --input out/session.fvblobs --extract-dir out/extracted
>ls -lh out/extracted | head -n 40
>sha256sum out/extracted/*.wav | head -n 20

csvtest: all
>./$(CSV_BIN) --input-csv out/sample-sensor.csv --out-prefix out/csv-session --threshold 1.65 --baseline-samples 160 --seed 0xC5C5C5C5
>./$(DUMP_BIN) --input out/csv-session.fvblobs --summary

audition:
>ffmpeg -hide_banner -y -f concat -safe 0 -i out/audition/list.txt -af "volume=2.5,alimiter=limit=0.95" -ar 48000 -ac 1 -c:a pcm_s16le out/audition-blobs.wav

test: all
>./$(TEST_BIN)
>./fv-recording-test
>./fv-fragment-test
>./fv-typography-test "$(TYPOGRAPHY_FONT)"
>./fv-camera-test
>./tests/test_smoke.sh

FRAGMENT_SRC := tests/test_fragment_engine.c ../android/app/src/main/cpp/fv_fragment_engine.c ../android/app/src/main/cpp/fv_fragment_store.c ../android/app/src/main/cpp/fv_sensor_dsp.c ../android/app/src/main/cpp/fv_recording_data.c ../android/app/src/main/cpp/fv_local_bank.c ../android/app/src/main/cpp/fv_vox_controls.c ../android/app/src/main/cpp/fv_stage1r_native_audio.c ../android/app/src/main/cpp/fv_station_policy.c ../android/app/src/main/cpp/fv_radio_playlist.c ../android/app/src/main/cpp/fv_environment_reverb.c
LVM_ROOT := ../android/app/src/main/cpp/third_party/lvm
ALGORITHM_SRC := $(wildcard $(LVM_ROOT)/Reverb/src/*.c $(LVM_ROOT)/Common/src/*.c) ../android/app/src/main/cpp/third_party/sonic/sonic.c
ALGORITHM_FLAGS := -DBUILD_FLOAT -DHIGHER_FS -I$(LVM_ROOT)/Reverb/lib -I$(LVM_ROOT)/Reverb/src -I$(LVM_ROOT)/Common/lib -I$(LVM_ROOT)/Common/src
ALGORITHM_OBJ := $(patsubst %.c,.audio-build/%.o,$(notdir $(ALGORITHM_SRC)))
ALGORITHM_HDR := $(wildcard $(LVM_ROOT)/Reverb/lib/*.h $(LVM_ROOT)/Reverb/src/*.h $(LVM_ROOT)/Common/lib/*.h $(LVM_ROOT)/Common/src/*.h) ../android/app/src/main/cpp/third_party/sonic/sonic.h
$(ALGORITHM_OBJ): $(ALGORITHM_HDR)
vpath %.c $(sort $(dir $(ALGORITHM_SRC)))
.audio-build/%.o: %.c
>mkdir -p .audio-build
>$(CC) -std=c23 -O2 -g $(ALGORITHM_FLAGS) -c $< -o $@
fv-fragment-test: $(FRAGMENT_SRC) $(ALGORITHM_SRC) $(ALGORITHM_OBJ) $(wildcard ../android/app/src/main/cpp/*.h)
>$(CC) $(CFLAGS) $(ALGORITHM_FLAGS) -o $@ $(FRAGMENT_SRC) $(ALGORITHM_OBJ) $(LDFLAGS) -pthread

fv-typography-test: tests/test_typography.c ../android/app/src/main/cpp/fv_gfx.c ../android/app/src/main/cpp/fv_gfx.h ../android/app/src/main/cpp/fv_android_text.c ../android/app/src/main/cpp/fv_android_text.h ../android/app/src/main/cpp/third_party/stb_truetype.h
>$(CC) -std=c23 -O2 -g -Wall -Wextra -Wpedantic -Wno-unused-function -o $@ tests/test_typography.c ../android/app/src/main/cpp/fv_gfx.c ../android/app/src/main/cpp/fv_android_text.c -lm

fv-camera-test: tests/test_camera_effects.c ../android/app/src/main/cpp/fv_camera_effects.c ../android/app/src/main/cpp/fv_camera_effects.h
>$(CC) $(CFLAGS) -o $@ tests/test_camera_effects.c ../android/app/src/main/cpp/fv_camera_effects.c $(LDFLAGS)

fv-station-blob-gen: src/station_blob_gen.c src/station_catalog.h
>$(CC) $(CFLAGS) -o $@ src/station_blob_gen.c

blobgen: fv-station-blob-gen
>./fv-station-blob-gen

clean:
>rm -f $(MAIN_BIN) $(DUMP_BIN) $(CSV_BIN) $(TEST_BIN) fv-recording-test fv-station-blob-gen
>rm -f fv-fragment-test fv-typography-test fv-camera-test
>rm -rf .audio-build
>rm -rf out
