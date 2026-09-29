# FieldVox Lab

FieldVox Lab is an experimental, audit-oriented Android ITC and sensor workstation. The Android app is implemented as a pure `android.app.NativeActivity`: project runtime code is C11 and Zig, station data is a CRC32-checked binary blob, and there are no project Java, Kotlin, or C++ runtime sources.

FieldVox does not claim that sensor changes or audio fragments prove paranormal communication. It records and displays physical signals so investigative sessions can be audited without obscuring where a sound or trigger originated.

## Current native feature set

- **512-slot verified station ring**: Binary station plan generated in Zig and embedded as a packed binary blob with CRC32 verification. Streams are verified direct live broadcasts (news, public radio, talk, NOAA weather, aviation, global shortwave relays).
- **GhostTube-style viewfinder GUI**: High-contrast darkroom HUD with interactive reticle, corner targeting brackets, status telemetry, and a slide-up controls drawer.
- **Real-time H.264 + AAC MP4 video recording**: Hardware-accelerated AMediaCodec video recording (H.264 AVC High Profile @ 720×1280, 30 fps, BT.709 studio limited swing, 3.5 Mbps) multiplexed with 48 kHz AAC audio in an MP4 container.
- **Hardware-aligned NV12 encoding**: Automatic query of codec input format (`AMEDIAFORMAT_KEY_STRIDE` = 768) preventing scanline shifting, chroma corruption, or edge artifacts on Android hardware encoders.
- **Camera2 NDK viewfinder pipeline**: Native 16:9 640×360 capture with hardware sensor orientation compensation (90°/180°/270°/0° bijective rotation) and dynamic range AGC low-light boost.
- **Camera buffer starvation protection & watchdog**: 8-buffer reader pool with queue-draining `acquireNextImage` loop and a 2000 ms frame liveness watchdog that automatically recycles stalled HAL sessions.
- **Spectral optical filters**: Real-time luma processing for **PHOSPHOR** (night-vision green), **THERMAL** (infrared heat map), **CYAN** (twilight spectral), and **STEALTH** (OLED black).
- **Multi-sensor VOX gating**: Real-time NDK sensor processing for magnetometer (EMF), gyroscope, accelerometer (geophone vibration), barometer (cold-spot pressure delta), and proximity detection.
- **Offline & live sweep modes**: Forward, reverse, and random sweep patterns with user-selectable cadences (100 ms, 250 ms, 500 ms, 1000 ms) and dual-deck carrier switching.
- **Native DSP audio chain**: Voice-band bandpass filter (300 Hz–3.4 kHz), 150 ms feedback echo, Schroeder reverb, granular pitch shifting, white noise generator, and reverse-block buffer processing.
- **Floating sound visualizer**: 256-point FFT spectral analysis driving a centered, real-time audio spectrum display.
- **Pre-roll hit capture (MARK HIT)**: Five-second native PCM ring buffer; tapping **MARK HIT** captures the preceding three seconds as a timestamped 48 kHz, 16-bit mono WAV file.
- **Full session WAV recording**: Continuous background audio logging to app-private storage.
- **Sensor-driven REM-pod tone synthesis**: Four-tier audible alert tones synthesized natively in Zig based on EMF deviation.
- **Azimuth-driven 3D stereo panning**: Maps compass orientation to equal-power stereo gains.
- **Estes blind mode**: AMOLED blackout screen with touch-based hit logging and a visible exit target.

## Controls

| Control | Action | Behavior |
| --- | --- | --- |
| `REC` / `STOP` | Bottom Bar | Toggles real-time H.264 + AAC MP4 video recording. |
| `MARK HIT` | Bottom Bar | Flushes the preceding 3 seconds of audio to a timestamped WAV file. |
| `LIVE` / `ARM` | Bottom Bar | Starts/stops the live radio sweep engine and carrier decks. |
| `PHOSPHOR` / `THERMAL` / `CYAN` / `STEALTH` | Header / Bar | Cycles through real-time viewfinder spectral filters. |
| `SETTINGS` | Bottom Bar | Toggles the slide-up controls drawer. |
| `CAL ZERO` | Drawer | Re-zeros magnetometer and barometric baseline references. |
| `CADENCE` | Drawer | Cycles sweep interval (100 ms, 250 ms, 500 ms, 1000 ms). |
| `DIR` | Drawer | Toggles sweep pattern: Forward (`FWD`), Reverse (`REV`), Random (`RND`). |
| `DUAL` | Drawer | Toggles dual simultaneous carrier sweepers. |
| `VOCAL` | Drawer | Toggles telephone voice-band pass filter (300 Hz–3.4 kHz). |
| `ECHO` | Drawer | Toggles 150 ms feedback delay. |
| `REVERB` | Drawer | Toggles native Schroeder reverberation. |
| `PITCH` | Drawer | Toggles granular pitch transposition. |
| `NOISE` | Drawer | Toggles background white-noise bed. |
| `REMPOD` | Drawer | Toggles proximity/EMF audio alert synth. |
| `3D PAN` | Drawer | Toggles compass-based stereo panning. |
| `ESTES` | Drawer | Enters blind listener mode with tap-to-mark hit logging. |
| `RECORD` | Drawer | Toggles continuous background WAV logging. |

## Media Specifications

### MP4 Video Recordings
- **Container**: ISO Base Media File Format (`.mp4`)
- **Video Track**: H.264 AVC High Profile (`avc1`)
  - Resolution: 720×1280 (Portrait) / 1280×720 (Landscape)
  - Frame Rate: 30 fps progressive
  - Colorimetry: BT.709 Studio Swing (`tv, bt709, progressive`)
  - Bitrate: 3.5 Mbps VBR
- **Audio Track**: AAC-LC (`mp4a`)
  - Sample Rate: 48,000 Hz mono
  - Bitrate: 128 kbps
- **Destination**: `/storage/emulated/0/Android/data/org.fieldvox.lab/files/fieldvox-recordings/session-<timestamp>.mp4`

### Audio Hit Captures (MARK HIT)
- **Format**: Broadcast WAV (`RIFF / WAVE fmt 1`)
- **Audio**: 48,000 Hz, 16-bit signed PCM mono
- **Duration**: 3.0 seconds pre-roll capture
- **Destination**: `/storage/emulated/0/Android/data/org.fieldvox.lab/files/fieldvox-hits/hit-<timestamp>.wav`

## Architecture

```text
source/src/station_blob_gen.zig
        │
        ├── android/app/src/main/assets/fieldvox_station_plan.bin (512 stations)
        └── android/app/src/main/cpp/fv_station_blob_data.h
                    │
                    ▼
android.app.NativeActivity (libfieldvox_native.so)
        │
        ├── fieldvox_native_activity.c   Lifecycle, touch widgets, HUD, drawer, worker threads
        ├── fv_native_video_recorder.c   NDK AMediaCodec (H.264/AAC) + AMediaMuxer (NV12 BT.709)
        ├── fv_native_camera.c           Camera2 NDK, AImageReader (640x360), rotation, AGC, watchdog
        ├── fv_gfx.c                     Anti-aliased software rasteriser, cover blit, text glow
        ├── fv_native_radio.c            Multi-deck HTTP streaming engine & carrier switching
        ├── fv_grain_gate.c              Sensor-triggered VOX grain windowing
        ├── fv_stage1r_native_audio.c    256-pt FFT, 5s ring buffer, Schroeder reverb, pitch shift
        └── fieldvox_zig_dsp.zig         Zig binary blob validator, REM tones, 3D panning gains
```

The Zig DSP module is cross-compiled to position-independent code (PIC) for each Android ABI (`arm64-v8a`, `x86_64`) and statically linked into `libfieldvox_native.so`.

## Build and Installation

### Requirements

- Linux (Arch Linux, Debian/Ubuntu, or compatible)
- Android SDK (API 30+) & NDK (r25+)
- Zig 0.13+ or compatible
- JDK 17+
- C11 compiler (`gcc` or `clang`) for host tests
- `adb` and `ffmpeg` (for media verification)

### Build Android APK

```bash
cd android
ANDROID_HOME=/home/baconfat/Android/Sdk ./gradlew :app:assembleDebug
```

The compiled APK will be at:
`android/app/build/outputs/apk/debug/app-debug.apk`

### Install and Run on Device

```bash
# Install to connected device
adb install -r android/app/build/outputs/apk/debug/app-debug.apk

# Launch NativeActivity
adb shell am start -n org.fieldvox.lab/android.app.NativeActivity
```

## Verification and Testing

### Host Native Test Suite

Run native C unit tests, Zig DSP tests, and end-to-end smoke tests:

```bash
make -C source clean
make -C source test
```

### Inspect Output Media on Device

```bash
# List recorded videos and hit WAVs
adb shell run-as org.fieldvox.lab ls -lh files/fieldvox-recordings
adb shell run-as org.fieldvox.lab ls -lh files/fieldvox-hits

# Pull newest recorded session
adb pull /storage/emulated/0/Android/data/org.fieldvox.lab/files/fieldvox-recordings/$(adb shell ls -1t /storage/emulated/0/Android/data/org.fieldvox.lab/files/fieldvox-recordings/ | head -n 1) session_test.mp4

# Verify stream parameters with ffprobe
ffprobe -hide_banner session_test.mp4
```

### Camera & Hardware Encoder Live Telemetry

```bash
# Monitor camera frame production and buffer queue
adb shell dumpsys media.camera | grep -E "Consumer name|Frames produced|currently dequeued"

# Monitor video recorder and native activity events
adb logcat -s FieldVoxVideoRec FieldVoxCamera FieldVoxNative
```

## Permissions and Privacy

- `CAMERA`: Required for real-time Camera2 viewfinder display and video recording.
- `RECORD_AUDIO`: Required for microphone monitoring and audio muxing into video sessions.
- `INTERNET`: Required for streaming live radio stations from verified online broadcast feeds.
- All session recordings remain strictly in app-private storage unless explicitly pulled via ADB or exported by the user.
- There are no word banks, no text-to-speech synthesizers, and no algorithmic phrase generators.

## License

See [source/LICENSE](source/LICENSE).
