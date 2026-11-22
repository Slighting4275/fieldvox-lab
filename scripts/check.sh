#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
FAST=0
if [[ "${1:-}" == "--fast" ]]; then
    FAST=1
elif [[ $# -gt 0 ]]; then
    echo "Usage: $0 [--fast]" >&2
    exit 2
fi

SPDX_HDR='/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */'
CPP_DIR="$ROOT/android/app/src/main/cpp"

echo "==> Checking SPDX headers on first-party C sources/headers"
for f in "$ROOT"/source/src/*.c "$ROOT"/source/src/*.h "$ROOT"/source/tests/*.c "$CPP_DIR"/*.c "$CPP_DIR"/*.h; do
    first="$(head -n 1 "$f")"
    if [[ "$first" != "$SPDX_HDR" ]]; then
        echo "ERROR: Missing combined SPDX header in $f" >&2
        exit 1
    fi
done

echo "==> Checking JNI helper & WAV header encapsulation"
if grep -rnE '\(\*env\)->(FindClass|GetMethodID|GetStaticMethodID|GetFieldID|GetStaticFieldID|ExceptionCheck|ExceptionClear)' \
    "$CPP_DIR"/*.c "$CPP_DIR"/*.h | grep -v '/fv_jni_helpers\.h:'; then
    echo "ERROR: Bare JNI lookup/exception boilerplate found outside fv_jni_helpers.h" >&2
    exit 1
fi

if grep -rn '"RIFF"' "$ROOT"/source/src "$CPP_DIR"/*.c "$CPP_DIR"/*.h \
    | grep -v '/fv_recording_data\.c:'; then
    echo "ERROR: Manual RIFF header construction found outside fv_recording_data.c" >&2
    exit 1
fi

echo "==> Checking for hardcoded /home/ paths in tracked sources/docs"
if grep -rnE '/home/[A-Za-z0-9._-]+' \
    "$ROOT/README.md" "$ROOT/AGENTS.md" "$ROOT/docs" "$ROOT/source/src" "$ROOT/source/tests" "$CPP_DIR" \
    --exclude-dir=third_party; then
    echo "ERROR: Hardcoded /home/<user> path found" >&2
    exit 1
fi

echo "==> Running host C23 build and test suite"
make -C "$ROOT/source" clean
make -C "$ROOT/source" test
make -C "$ROOT/source" clean

if [[ "$FAST" -eq 0 ]]; then
    SDK_DIR="${ANDROID_HOME:-$HOME/Android/Sdk}"
    if [[ ! -d "$SDK_DIR" ]]; then
        echo "ERROR: ANDROID_HOME not found at $SDK_DIR" >&2
        exit 1
    fi
    echo "==> Running Android NDK C23 debug APK build"
    ANDROID_HOME="$SDK_DIR" "$ROOT/android/gradlew" -p "$ROOT/android" --console=plain :app:assembleDebug

    BUILT_APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
    DIST_APK="$ROOT/apk/fieldvox-lab-debug.apk"
    if ! cmp -s "$BUILT_APK" "$DIST_APK"; then
        echo "ERROR: $DIST_APK is out of sync with $BUILT_APK; copy APK and update apk/SHA256SUMS" >&2
        exit 1
    fi
    (cd "$ROOT/apk" && sha256sum -c SHA256SUMS)
fi

echo "All FieldVox checks passed."
