# FieldVox Lab

FieldVox Lab is a clean-room open-source experimental Android and Linux project for environmental-sensor-triggered audio session recording.

It records local session artifacts such as JSONL event logs, WAV audio, exported session pairs, and readable session manifests. The Android app includes controls for session recording, app-private session browsing, replace-safe public export to Downloads/FieldVoxLab, share actions using Android content URIs, and TSV manifest export.

## Current Android checkpoint

Stage 1J-A adds Export Manifest support.

The Ctrl panel includes:

- Export All Sessions
- Share All Sessions
- Export Manifest

Export Manifest creates fieldvox-session-manifest.tsv from matched JSONL/WAV session pairs and exports it to public Downloads/FieldVoxLab.

## Project status

Verified on Pixel 7 / GrapheneOS:

- Android package: org.fieldvox.lab
- Version: 0.1.0-stage1j-a-export-session-manifest
- APK SHA256: cf1c0e87ab05baa28f33bd3952204e894ec7d3a4c83662f7da9124ff0ab98c38
- Manifest export verified with private/public byte-for-byte match
- Public JSONL/WAV exports verified byte-for-byte against app-private session files
- No broad Android storage permissions are used

## Safety and scope

FieldVox Lab is experimental. It does not prove paranormal activity, spirit communication, or any supernatural claim.

Do not use this tool for important life, health, legal, financial, grief, safety, or emergency decisions.

## Clean-room policy

This project uses its own name, UI, code, assets, and data. It does not include proprietary branding, proprietary assets, proprietary databases, or copied marketing copy from other apps.

## Repository layout

- android/: Android app
- source/: Linux C prototype/tools

Internal checkpoint notes, release archives, build logs, and local database exports are intentionally kept out of public Git.

## Build notes

Android:

    cd android
    ./gradlew :app:assembleDebug

Linux prototype/tools:

    cd source
    make

## License

GPL-3.0-or-later. See LICENSE for the project notice and COPYING for the full GNU GPL version 3 text.
