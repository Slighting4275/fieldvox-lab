# FieldVox Lab

C23 workstation and Android NativeActivity app (GPL-3.0-or-later AND AGPL-3.0-or-later).

## Key invariants & non-obvious layout

- **Two build graphs:** Host tests (`make -C source test`) do **not** compile Android/JNI sources (`fieldvox_native_activity.c`, `fv_native_camera.c`, `fv_native_radio.c`, `fv_local_player.c`, `fv_platform_voices.c`, `fv_android_text.c`). Run `./scripts/check.sh` before finishing NDK changes (`--fast` for pre-commit host/static checks only).
- **Generated catalog:** Do not hand-edit `android/app/src/main/cpp/fv_station_blob_data.h`; regenerate it via `make -C source blobgen` from `source/src/station_blob_gen.c` and `source/src/station_catalog.h`.
- **APK sync:** After `:app:assembleDebug` without `--offline` (only use `--offline` once Gradle caches are populated), copy `android/app/build/outputs/apk/debug/app-debug.apk` to `apk/fieldvox-lab-debug.apk` and refresh `apk/SHA256SUMS`.
- **Standards & license:** Read `docs/CODING_STANDARDS.md` (judgement calls for reviews) and `docs/COMBINED_GPL_AGPL_LICENSE_POLICY.md`.
