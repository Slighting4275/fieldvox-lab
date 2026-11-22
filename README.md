# FieldVox Lab

FieldVox Lab gives you a sensor-triggered spirit box, a filtered camera view, and session recording in one native Android app. Motion and magnetic field readings can trigger short fragments from online radio, or you can use cached radio clips when you’re offline. Switch to Estes blackout mode and tap to mark moments while the screen stays dark.

Hear something through the static that makes you stop and listen? Tap Mark Hit to save up to three seconds of audio from before you tapped, along with the source details and sensor readings. Record the hunt on video, then jump straight to marked events in the built-in library.

## New audio features

- **LIVE and LOCAL playback.** LIVE requests short online radio clips from a shuffled group of ten US English sources. The group includes US-profile talk sources and a shared English source; profile membership is not restricted by the broadcaster’s country. Sensor digits choose a live request or a cached clip. Failed requests retain their cooldown instead of silently walking to another station. LOCAL plays cached radio clips without network connections, or fragments from a saved PCM WAV selected in the library.
- **Independent pitch and speed.** Fragments use pitch ratios of 0.6×, 1.0×, or 1.2× and playback speeds of 0.7×, 1.0×, or 1.2×. The default speaker path uses Android `MediaPlayer` with separate `PlaybackParams` pitch and speed settings on the whole compressed source, avoiding the former short-grain transform and mono downmix before playback.
- **Ten playback slots.** A C23 JNI backend owns ten Android players and four preparation workers, with an independent stop timer. Echoes acquire a slot when their scheduled time arrives. Main slots remain reserved through the configured reverb tail; a full pool rejects additional fragments rather than replacing a playing voice.
- **Four finite echo repeats.** Repeats play at gains of 0.06, 0.018, 0.0054, and 0.00162. The Echo slider sets the interval between repeats to 200 ms × its level.
- **Platform environmental reverb.** Primary voices use a shared session-0 `EnvironmentalReverb`, 50 ms reverb delay, full density, diffusion scaled by mix, and a requested decay of 9,000 ms × mix. Echo voices remain dry. Device effects can reject settings: the Pixel test accepted 4,500 and 6,750 ms but rejected 9,000 ms, retaining the previous decay. Slot reservation still follows the requested tail.
- **Persistent offline cache.** LIVE downloads up to 50,000 compressed bytes with bounded network timeouts, retains the original compressed source for playback, and saves it alongside a mono PCM companion for native analysis and recordings. Clip duration depends on the broadcaster’s bitrate, with a 30-second safety cap. Memory and disk caches each hold up to 100 clips, evict the oldest clips, and limit each cached clip to five retrievals per run. Cache eviction does not delete session recordings or marked hits.
- **Sensor-derived fragment selection.** Magnetometer magnitudes and accelerometer/gyroscope squared magnitudes supply decimal digits for gating, pitch, speed, source choice, seek position, and source lengths of 400–760 ms. Distortion adjusts pitch/speed selection thresholds instead of applying clipping.
- **Playback cooldowns.** Live requests start a reverb-dependent cooldown of 2–10 seconds before downloading. A five-second cooldown limits repeated live requests to the same station. Cached playback selects one clip without substituting another exhausted slot.
- **Speaking mute.** An optional microphone detector uses a Hann-windowed packed-real FFT to measure energy in the 80–300 Hz band, suppresses new fragments during detection and for two seconds afterward, and fades speaker monitoring. This is a frequency-band detector, not speech recognition.
- **Timed static bursts.** Optional low-level white noise follows the selected sweep interval with a 40 ms gap between bursts.

Sensor-only triggering is the default. AUTO adds timer-driven requests. Pitch defaults to sensor selection and can be overridden with LOW, NORMAL, or HIGH.

Audio comes from broadcast streams or selected WAV files. FieldVox does not use word banks, text-to-speech, or generated phrases. Sensor readings and perceived audio messages are not evidence of paranormal communication.

## Camera, sensors, and display

- **Platform typography.** C23 JNI uses Android’s system sans-serif typefaces with real regular, medium, and bold weights. Controls/status labels use 12 sp, small values 8 sp, the controls title 14 sp, and library filenames 16 sp with 11 sp details. Android converts sp using the device’s font settings, including nonlinear scaling. Long filenames are elided at the declared size while retaining their file type and capture suffix; a bounded glyph cache prevents text from disappearing as sizes change.
- **Screen stays awake in the foreground.** Camera monitoring, controls, recording, and library replay remain visible without the normal display timeout. Leaving the app restores normal timeout behavior; the power button can still lock the phone. Estes mode retains its intentional dark display.
- **Camera swipes.** Swipe left on the clear camera preview for the next filter, or right for the previous one. Both directions wrap through NORMAL, GREEN, PURPLE, DEPTH, HEAT, and INVERTED. Horizontal travel must exceed 100 physical pixels and recent horizontal velocity must exceed 100 pixels/second; horizontal travel must dominate vertical travel. Recording, disabled/stalled cameras, open overlays, sliders, buttons, canceled gestures, and multiple fingers suppress camera swipes. The LENS button also advances the same cycle; the advanced camera switch controls STEALTH separately.
- **Full-color camera processing in C23.** Camera2 NDK preserves validated YUV chroma, converts to RGB, and applies the selected filter before orientation correction and display. Green/Purple use RGB color matrices with a 4× boost when the torch is on. Depth uses a bilinearly sampled nine-tap neighborhood, its declared divisor of 100, stepped grayscale and 1.6× brightness. Heat interpolates blue/yellow/red from RGB intensity; Inverted reverses each color channel. Depth and Heat are visual effects, not depth or temperature measurements. The camera retains its stalled-frame watchdog.
- Magnetometer, accelerometer, gyroscope, proximity, and barometer telemetry, where supported by the device.
- Spectrum, PCM waveform, and spectral history views with selectable trace colors.
- Slide-up audio controls, advanced sensor controls, haptic feedback, torch control, and an in-app field guide.
- Optional voice-band filtering, reversed audio, sensor-driven REM tones, compass-based stereo panning, and an Estes blackout mode with tap-to-mark logging.

The embedded 289-entry station catalog is generated in C23 and verified with CRC32 at startup. US English has 17 eligible entries, shuffled down to ten per process. The catalog is a bundled snapshot; it is not synchronized with another application’s server updates. Availability depends on the broadcaster and network.

## Recording and library

Speaker playback and recordings use different paths. Default radio voices play through Android’s player/effect chain. WAV/MP4 captures, hit files, and visualizers use a parallel mono C reconstruction with Sonic/LVREV; they are **not an exact capture of platform speaker output**. VOCAL, reverse, REM tones, and 3D panning select the native speaker path so those PCM tools remain audible. The native path also provides a fallback if platform player initialization or primary playback fails.

**MARK HIT** saves up to three seconds of available pre-roll as two aligned 48 kHz, 16-bit mono WAV files and a JSON timeline:

- `hit-<timestamp>-<sequence>-clean.wav`: pitched, tempo-adjusted primary fragments plus microphone audio, before echo, reverb, static, and alert tones.
- `hit-<timestamp>-<sequence>-preroll.wav`: processed recording audio before speaker mute and stereo panning.
- `hit-<timestamp>-<sequence>.json`: historical source and trigger metadata, sensor readings, effect settings, pitch ratio, playback speed, grain length, seek fraction, cached-source flag, and active voice count.

The timeline records the audio blocks actually included in the WAVs. Its completion flag indicates metadata coverage. Pausing resets pre-roll, and newly started captures save only the audio available.

Tap the red **REC** button to start a **Clean MP4**, then tap **STOP** to save it. Clean mode saves the filtered camera view, session audio, and timeline markers. The camera view retains its proportions and is cropped to fill the MP4 frame. Select **Screen MP4** above the same red button to include FieldVox's on-screen controls and overlays.

If the camera is starting, Clean MP4 waits for the preview after you press **REC**. Tap the shutter again to cancel while it waits. Entering Estes during a recording keeps the display dark and shows the active recording mode in the title. Clean MP4 continues saving the camera view during Estes.

Both modes record H.264 video at up to 30 fps and 3.5 Mbps, with BT.709 limited-range color and 48 kHz mono AAC audio at 128 kbps. Capture timestamps follow the actual frame cadence.

**LIBRARY** lists saved WAV and MP4 captures. Playback shows a waveform with labeled markers for VOX fragments, REM-POD alerts, manual hits, and bookmarks added during review. Use **< MARK** and **MARK >** to jump between events. Older recordings keep their saved markers. **BANK** selects a saved PCM WAV as the LOCAL fragment source. **PLAY** replays the recording. Explicit exports use Android MediaStore under `Music/FieldVox` or `Movies/FieldVox`.

Recordings, hit captures, and the radio clip cache use separate app-private directories:

| Content | Directory relative to the app's private storage |
| --- | --- |
| Session recordings | `files/fieldvox-recordings/` |
| Marked hits and JSON timelines | `files/fieldvox-hits/` |
| Automatically cached radio clips | `files/fieldvox-grain-cache-v2/` |

The current cache stores paired `.wav` and `.wav.audio` files, counted as one clip. Eviction also removes the paired original. The previous cache directory is left intact during updates; its station numbers are not reused with the new catalog.

## Main controls

| Control | Purpose |
| --- | --- |
| `RUNNING / STOPPED` | Start or stop fragment playback, static bursts, and effect tails. Cached clips and selected effect levels are retained. |
| `LIVE / LOCAL` | Choose online radio with sensor-selected cached clips or offline playback. |
| `AUTO / SENSOR` | Enable timer-plus-sensor requests or sensor-only requests. |
| `ECHO`, `REVERB`, `DISTORTION` | Adjust repeat spacing, reverb mix, and sensor pitch/speed selection. |
| `RADIO SWEEP RATE` / `CADENCE` | Set sweep/static timing and optional AUTO source length. Sensor source lengths remain 400–760 ms. |
| `PITCH` | Cycle AUTO, LOW, NORMAL, and HIGH. |
| `SPEAKING MUTE` | Enable microphone-based fragment suppression and speaker fading. |
| `WHITE NOISE` / `NOISE` | Enable timed static bursts. |
| `MUTE / MUTED` | Silence speaker monitoring while recording continues. |
| `MARK HIT` | Save paired pre-roll audio and its timeline. |
| `REC / STOP` | Start or stop video in the selected MP4 mode. |
| `LIBRARY`, `PLAY`, `BANK` | Browse, replay, or select a saved WAV for fragments. |
| `LENS`, visualizer arrows and color | Select camera and audio display styles. |
| `CAL ZERO`, sensitivity and cooldown presets | Adjust sensor baseline and auxiliary sensor trigger settings. |
| `VOCAL`, reverse, `REMPOD`, `3D PAN`, `ESTES` | Enable optional audio and investigation tools. |

Echo, reverb, and distortion start at 50%, with a 200 ms sweep interval. Speaking mute is off by default. Auxiliary sensor presets are separate from the decimal magnetometer/accelerometer/gyroscope gate and the audio engine's admission cooldowns.

## Download the APK

[Download FieldVox Lab 0.5.2-native](https://gitlab.com/exalted6165/fieldvox-lab/-/raw/main/apk/fieldvox-lab-debug.apk) (from the public GitLab mirror; canonical source on [Codeberg](https://codeberg.org/Exalted6165/fieldvox-lab)). This is a signed debug build for Android 11 or newer, with ARM64 and x86_64 support. Its application ID is `org.fieldvox.lab` and its version code is `9`.

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
ANDROID_HOME=/path/to/Android/Sdk ./android/gradlew -p android :app:assembleDebug
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n org.fieldvox.lab/android.app.NativeActivity
```

Only pass `--offline` after an online build has cached Gradle plugins and dependencies locally. Installing with `-r` preserves app data. The APK is written to `android/app/build/outputs/apk/debug/app-debug.apk`.

## Verification

Run the host C23 unit, recording, fragment-engine, typography, camera, and simulator smoke tests:

```bash
make -C source test
```

The fragment tests cover independent pitch/speed conversion, echo timing and gain, speech suppression, admission cooldowns, voice capacity, cache limits, paired-source persistence, descriptor ownership, backend rejection, regional language selection, and invalid inputs. Typography tests use the installed sans font reported by `fc-match`; set `TYPOGRAPHY_FONT=/path/to/font.ttf` when needed. Camera tests exercise production RGB filter functions, chroma preservation, rotation, bounds, both swipe directions, thresholds, hold/cancel handling and wraparound. Host tests do not replace camera, codec, sensor, or lifecycle testing on an Android device.

Inspect a connected device and follow runtime diagnostics:

```bash
adb devices -l
adb shell pidof org.fieldvox.lab
adb logcat -v time -s FieldVoxNative FieldVoxRadio FieldVoxVoices FieldVoxCamera FieldVoxVideoRec
adb shell run-as org.fieldvox.lab ls -lh files/fieldvox-recordings
adb shell run-as org.fieldvox.lab ls -lh files/fieldvox-hits
```

Use `ffprobe -hide_banner <recording.mp4>` to inspect an exported recording's actual media properties.

## Implementation

The application runtime is written in **C23**, using Android's `NativeActivity`, Camera2 NDK, AAudio, native media codecs, and Android media/effect APIs called through C JNI. There are no project Java, Kotlin, or C++ runtime sources.

| Module | Responsibility |
| --- | --- |
| `fieldvox_native_activity.c` | Activity lifecycle, controls, camera HUD, sensors, audio integration, and recording. |
| `fv_native_radio.c` | Bounded on-demand HTTP clip fetches, compressed-source ownership, cancellation and native decoding. |
| `fv_android_text.c`, `fv_gfx.c` | Platform typeface rasterization through C JNI, sp conversion, metrics, and bounded text caches. |
| `fv_platform_voices.c` | C JNI Android players, pitch/speed, platform reverb, finite echo scheduling and playback deadlines. |
| `fv_fragment_engine.c` | Fragment selection, independent pitch/tempo, voice slots, finite echoes, reverb, and static. |
| `fv_station_policy.c` | Language filtering and shuffled ten-station groups. |
| `fv_environment_reverb.c` | Android LVREV C reverb integration. |
| `fv_fragment_store.c` | Background clip persistence, reload, and FIFO eviction. |
| `fv_sensor_dsp.c` | Decimal sensor parameters, station-blob validation, REM tones, and panning gains. |
| `fv_vox_controls.c` | Microphone voice-band detection and control helpers. |
| `fv_hit_capture.c` | Paired pre-roll audio and source/effect timelines. |
| `fv_local_bank.c`, `fv_local_player.c` | Selected-WAV fragments and in-app recording replay. |
| `fv_native_camera.c`, `fv_native_video_recorder.c` | Native camera capture and H.264/AAC MP4 recording. |
| `fv_camera_effects.c` | Six RGB camera filters, bounded rotation and recent-sample swipe velocity/admission. |
| `source/` | Host simulator, station generator, utilities, and regression tests. |

All native application translation units, including vendored Sonic and LVREV sources, compile as C23. The vendored components retain their Apache-2.0 notices and upstream identities under `android/app/src/main/cpp/third_party/`. The fragment engine controls admissions and native recording reconstruction; the default speaker backend uses Android players and environmental reverb through C JNI. Older continuous DSP helpers remain available for legacy tests. No Zig compiler or runtime is required.

## Permissions and storage

Camera access supports the viewfinder and video recording. Microphone access is requested for features that need microphone input; radio-only playback and recording do not require it. Network access is used for LIVE radio. Recordings remain in app-private storage until the user exports or copies them.

## License

See [source/LICENSE](source/LICENSE).
