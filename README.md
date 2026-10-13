# FieldVox Lab

FieldVox Lab is an experimental Android workstation for sensor-triggered radio audio, offline fragment playback, camera monitoring, and session recording. Its application runtime is written in **C23**, using Android's `NativeActivity`, Camera2 NDK, AAudio, and native media codecs. There are no project Java, Kotlin, or C++ runtime sources.

## New audio features

- **LIVE and LOCAL playback.** LIVE buffers online radio across up to ten decks and uses cached clips when streams are unavailable. LOCAL plays cached radio clips without network connections, or fragments from a saved PCM WAV selected in the library.
- **Independent pitch and speed.** Fragments use pitch ratios of 0.6×, 1.0×, or 1.2× and playback speeds of 0.7×, 1.0×, or 1.2×. Speed changes playback duration independently of pitch.
- **Ten playback slots.** Each admitted fragment gets its own immutable audio buffer. Busy slots remain reserved through the configured reverb tail; a full pool rejects additional fragments rather than replacing a playing voice.
- **Four finite echo repeats.** Repeats play at gains of 0.06, 0.018, 0.0054, and 0.00162. The Echo slider sets the interval between repeats to 200 ms × its level.
- **Native reverb.** A floating-point reverberator provides 50 ms pre-delay, mix-dependent diffusion, and decay up to nine seconds. It approximates an environmental reverb effect; it is not a measured model of a particular room.
- **Persistent offline cache.** LIVE saves decoded four-second PCM clips in a dedicated cache. Memory and disk caches each hold up to 100 clips, evict the oldest clips, and limit each cached clip to five playback admissions per run. Cache eviction does not delete session recordings or marked hits.
- **Sensor-derived fragment selection.** Magnetometer and accelerometer vector magnitudes supply decimal digits for gating, pitch, speed, source choice, seek position, and source lengths of 400–760 ms. Distortion adjusts pitch/speed selection thresholds instead of applying clipping.
- **Playback cooldowns.** Accepted fragments share a reverb-dependent cooldown of 2–10 seconds. A five-second cooldown limits repeated use of the same station.
- **Speaking mute.** An optional microphone detector measures energy in the 80–300 Hz band, suppresses new fragments during detection and for two seconds afterward, and fades speaker monitoring. This is a frequency-band detector, not speech recognition.
- **Timed static bursts.** Optional low-level white noise follows the selected sweep interval with a 40 ms gap between bursts.

Sensor-only triggering is the default. AUTO adds timer-driven requests. Pitch defaults to sensor selection and can be overridden with LOW, NORMAL, or HIGH.

Audio comes from broadcast streams or selected WAV files. FieldVox does not use word banks, text-to-speech, or generated phrases. Sensor readings and perceived audio messages are not evidence of paranormal communication.

## Camera, sensors, and display

- Camera2 NDK viewfinder with orientation correction, low-light luma boost, and a stalled-frame watchdog.
- PHOSPHOR, THERMAL, CYAN, and STEALTH display filters. These are visual treatments of ordinary camera images; THERMAL is a false-color display, not a temperature measurement.
- Magnetometer, accelerometer, gyroscope, proximity, and barometer telemetry, where supported by the device.
- Spectrum, PCM waveform, and spectral history views with selectable trace colors.
- Slide-up audio controls, advanced sensor controls, haptic feedback, torch control, and an in-app field guide.
- Optional voice-band filtering, reversed audio, sensor-driven REM tones, compass-based stereo panning, and an Estes blackout mode with tap-to-mark logging.

The embedded 512-entry station catalog is generated in C23 and verified with CRC32 at startup. Availability depends on the broadcaster and network.

## Recording and library

**MARK HIT** saves up to three seconds of available pre-roll as two aligned 48 kHz, 16-bit mono WAV files and a JSON timeline:

- `hit-<timestamp>-<sequence>-clean.wav`: pitched, tempo-adjusted primary fragments plus microphone audio, before echo, reverb, static, and alert tones.
- `hit-<timestamp>-<sequence>-preroll.wav`: processed recording audio before speaker mute and stereo panning.
- `hit-<timestamp>-<sequence>.json`: historical source and trigger metadata, sensor readings, effect settings, pitch ratio, playback speed, grain length, seek fraction, cached-source flag, and active voice count.

The timeline records the audio blocks actually included in the WAVs. Its completion flag indicates metadata coverage. Pausing resets pre-roll, and newly started captures save only the audio available.

**SESS WAV** records continuous session audio. **REC / STOP** records camera video as H.264 with AAC audio in MP4: up to 30 fps, 3.5 Mbps video, BT.709 limited-range color, and 48 kHz mono audio at 128 kbps. Capture timestamps follow the actual frame cadence.

**LIBRARY** lists saved WAV and MP4 captures. In-app replay supports seeking, WAV waveforms, and bookmarks. **BANK** selects a saved PCM WAV as the LOCAL fragment source; **PLAY** replays the recording. Explicit exports use Android MediaStore under `Music/FieldVox` or `Movies/FieldVox`.

Recordings, hit captures, and the radio clip cache use separate app-private directories:

| Content | Directory relative to the app's private storage |
| --- | --- |
| Session recordings | `files/fieldvox-recordings/` |
| Marked hits and JSON timelines | `files/fieldvox-hits/` |
| Automatically cached radio clips | `files/fieldvox-grain-cache/` |

## Main controls

| Control | Purpose |
| --- | --- |
| `RUNNING / STOPPED` | Start or stop fragment playback. |
| `LIVE / LOCAL` | Choose online radio with cached fallback or offline playback. |
| `AUTO / SENSOR` | Enable timer-plus-sensor requests or sensor-only requests. |
| `ECHO`, `REVERB`, `DISTORTION` | Adjust repeat spacing, reverb mix, and sensor pitch/speed selection. |
| `RADIO SWEEP RATE` / `CADENCE` | Set sweep/static timing and optional AUTO source length. Sensor source lengths remain 400–760 ms. |
| `PITCH` | Cycle AUTO, LOW, NORMAL, and HIGH. |
| `SPEAKING MUTE` | Enable microphone-based fragment suppression and speaker fading. |
| `WHITE NOISE` / `NOISE` | Enable timed static bursts. |
| `MUTE / MUTED` | Silence speaker monitoring while recording continues. |
| `MARK HIT` | Save paired pre-roll audio and its timeline. |
| `REC / STOP`, `SESS WAV` | Record MP4 video or session WAV audio. |
| `LIBRARY`, `PLAY`, `BANK` | Browse, replay, or select a saved WAV for fragments. |
| `LENS`, visualizer arrows and color | Select camera and audio display styles. |
| `CAL ZERO`, sensitivity and cooldown presets | Adjust sensor baseline and auxiliary sensor trigger settings. |
| `VOCAL`, reverse, `REMPOD`, `3D PAN`, `ESTES` | Enable optional audio and investigation tools. |

Echo, reverb, and distortion start at 50%, with a 200 ms sweep interval. Speaking mute is off by default. Auxiliary sensor presets are separate from the decimal magnetometer/accelerometer gate and the audio engine's admission cooldowns.

## Download the APK

[Download FieldVox Lab 0.3.0-native](https://codeberg.org/Exalted6165/fieldvox-lab/raw/branch/main/apk/fieldvox-lab-debug.apk). This is a signed debug build for Android 11 or newer, with ARM64 and x86_64 support. Its application ID is `org.fieldvox.lab` and its version code is `3`.

The APK and its SHA-256 checksum are stored under [`apk/`](apk/). Download it to the phone and open it to install or update FieldVox. Android may ask you to allow installation from the browser or file manager. Updating a compatible signed installation preserves app data.

Verify a checkout's packaged APK from the repository root:

```bash
cd apk
sha256sum -c SHA256SUMS
```

## Build and install

Requirements: a C23-capable host compiler, Android SDK platform/build tools 37, NDK `30.0.16248370`, CMake `4.1.2`, a compatible JDK, and ADB. Minimum Android version is Android 11 (API 30). Builds target `arm64-v8a` and `x86_64`.

Run from the repository root, using your Android SDK path:

```bash
ANDROID_HOME=/home/baconfat/Android/Sdk ./android/gradlew -p android :app:assembleDebug
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n org.fieldvox.lab/android.app.NativeActivity
```

Add `--offline` to the Gradle command when its dependencies are already cached. Installing with `-r` preserves app data. The APK is written to `android/app/build/outputs/apk/debug/app-debug.apk`.

## Verification

Run the host C23 unit, recording, fragment-engine, and simulator smoke tests:

```bash
make -C source test
```

The fragment tests cover independent pitch/speed conversion, echo timing and gain, speech suppression, admission cooldowns, voice capacity, cache limits, persistence, and invalid inputs. Host tests do not replace camera, codec, sensor, or lifecycle testing on an Android device.

Inspect a connected device and follow runtime diagnostics:

```bash
adb devices -l
adb shell pidof org.fieldvox.lab
adb logcat -v time -s FieldVoxNative FieldVoxRadio FieldVoxCamera FieldVoxVideoRec
adb shell run-as org.fieldvox.lab ls -lh files/fieldvox-recordings
adb shell run-as org.fieldvox.lab ls -lh files/fieldvox-hits
```

Use `ffprobe -hide_banner <recording.mp4>` to inspect an exported recording's actual media properties.

## Implementation

| Module | Responsibility |
| --- | --- |
| `fieldvox_native_activity.c` | Activity lifecycle, controls, camera HUD, sensors, audio integration, and recording. |
| `fv_native_radio.c` | Ten-deck network decoding and PCM buffering. |
| `fv_fragment_engine.c` | Fragment selection, independent pitch/tempo, voice slots, finite echoes, reverb, and static. |
| `fv_fragment_store.c` | Background clip persistence, reload, and FIFO eviction. |
| `fv_sensor_dsp.c` | Decimal sensor parameters, station-blob validation, REM tones, and panning gains. |
| `fv_vox_controls.c` | Microphone voice-band detection and control helpers. |
| `fv_hit_capture.c` | Paired pre-roll audio and source/effect timelines. |
| `fv_local_bank.c`, `fv_local_player.c` | Selected-WAV fragments and in-app recording replay. |
| `fv_native_camera.c`, `fv_native_video_recorder.c` | Native camera capture and H.264/AAC MP4 recording. |
| `source/` | Host simulator, station generator, utilities, and regression tests. |

All native application translation units require C23. The production audio path uses the fragment engine; older continuous DSP helpers remain available for legacy tests. No Zig compiler or runtime is required.

## Permissions and storage

Camera access supports the viewfinder and video recording. Microphone access is requested for features that need microphone input; radio-only playback and recording do not require it. Network access is used for LIVE radio. Recordings remain in app-private storage until the user exports or copies them.

## License

See [source/LICENSE](source/LICENSE).
