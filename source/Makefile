.RECIPEPREFIX := >

CC ?= cc
CFLAGS ?= -std=c23 -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
LDFLAGS ?= -lm

MAIN_BIN := fieldvox
DUMP_BIN := fv-blobdump
CSV_BIN := fv-csvvox
TEST_BIN := fv-unit-test
TYPOGRAPHY_FONT ?= $(shell fc-match -f "%{file}" sans 2>/dev/null)

MAIN_SRC := src/main.c src/fv_core.c ../android/app/src/main/cpp/fv_recording_data.c
DUMP_SRC := src/fv_blobdump.c src/fv_core.c ../android/app/src/main/cpp/fv_recording_data.c
CSV_SRC := src/fv_csvvox.c src/fv_core.c ../android/app/src/main/cpp/fv_recording_data.c
TEST_SRC := tests/test_native_units.c ../android/app/src/main/cpp/fv_stage1r_native_audio.c ../android/app/src/main/cpp/fv_grain_gate.c ../android/app/src/main/cpp/fv_hit_capture.c ../android/app/src/main/cpp/fv_vox_controls.c ../android/app/src/main/cpp/fv_sensor_dsp.c ../android/app/src/main/cpp/fv_local_bank.c ../android/app/src/main/cpp/fv_recording_data.c ../android/app/src/main/cpp/fv_ui_layout.c
HDR := src/fv_core.h

.PHONY: all clean run inspect dump extract csvtest audition test blobgen

all: $(MAIN_BIN) $(DUMP_BIN) $(CSV_BIN) $(TEST_BIN) fv-recording-test fv-fragment-test fv-typography-test fv-camera-test

$(MAIN_BIN): $(MAIN_SRC) $(HDR)
>$(CC) $(CFLAGS) -o $@ $(MAIN_SRC) $(LDFLAGS)

$(DUMP_BIN): $(DUMP_SRC) $(HDR)
>$(CC) $(CFLAGS) -o $@ $(DUMP_SRC) $(LDFLAGS)

$(CSV_BIN): $(CSV_SRC) $(HDR)
>$(CC) $(CFLAGS) -o $@ $(CSV_SRC) $(LDFLAGS)

$(TEST_BIN): $(TEST_SRC) $(wildcard ../android/app/src/main/cpp/*.h)
>$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(LDFLAGS)

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

FRAGMENT_SRC := tests/test_fragment_engine.c ../android/app/src/main/cpp/fv_fragment_engine.c ../android/app/src/main/cpp/fv_fragment_store.c ../android/app/src/main/cpp/fv_sensor_dsp.c ../android/app/src/main/cpp/fv_recording_data.c ../android/app/src/main/cpp/fv_local_bank.c ../android/app/src/main/cpp/fv_vox_controls.c ../android/app/src/main/cpp/fv_stage1r_native_audio.c ../android/app/src/main/cpp/fv_station_policy.c ../android/app/src/main/cpp/fv_environment_reverb.c
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

fv-camera-test: tests/test_camera_effects.c ../android/app/src/main/cpp/fv_camera_effects.c ../android/app/src/main/cpp/fv_camera_effects.h ../android/app/src/main/cpp/fv_recording_data.c ../android/app/src/main/cpp/fv_recording_data.h
>$(CC) $(CFLAGS) -o $@ tests/test_camera_effects.c ../android/app/src/main/cpp/fv_camera_effects.c ../android/app/src/main/cpp/fv_recording_data.c $(LDFLAGS)

fv-station-blob-gen: src/station_blob_gen.c src/station_catalog.h
>$(CC) $(CFLAGS) -o $@ src/station_blob_gen.c

blobgen: fv-station-blob-gen
>./fv-station-blob-gen

clean:
>rm -f $(MAIN_BIN) $(DUMP_BIN) $(CSV_BIN) $(TEST_BIN) fv-recording-test fv-station-blob-gen
>rm -f fv-fragment-test fv-typography-test fv-camera-test
>rm -rf .audio-build
>rm -rf out
