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

FieldVox Lab is free software licensed under both the GNU General Public License version 3 or later and the GNU Affero General Public License version 3 or later.

This is an AND-style combined copyleft policy, not an OR-choice. The intent is that forks, redistributed versions, modified versions, and hosted/network-service versions remain free and open source.

See `LICENSE` for the full combined notice and full license texts. See `COPYING` for the GNU GPL text, `COPYING.AGPL` for the GNU AGPL text, and `docs/COMBINED_GPL_AGPL_LICENSE_POLICY.md` for the project policy.

<!-- FIELDVOX_CURRENT_FEATURES_START -->
## Current Android features

Verified checkpoint: **Stage 1P-A native sweep scheduler audit**
Current Codeberg `main`: `efbc0a6` / `efbc0a6140aef81f1c4cd68584984ce7c1eb2454`
Latest commit subject: `Add Stage 1P-A native sweep scheduler audit`

FieldVox Lab is a clean-room experimental field-audio tool. It records sessions, marks user-perceived hits, exports blind-review bundles, and keeps source/audit metadata for later review. It is not evidence of paranormal activity by itself and should not be used for health, safety, legal, financial, grief, or other important life decisions.

### Live VOX session engine

- Android app package: `org.fieldvox.lab`.
- Multi-sensor VOX gates using environmental sensor state plus microphone recording state.
- Timeout-safe live carrier rotation so stalled stream preparation does not block queued gates.
- App-private session recording with WAV audio and JSONL session logs.
- Session browser, refresh, export, and share flows.
- Export path uses Android public Downloads through the app export flow; no broad storage permission is required.

### Evidence-grade hit marker and blind review export

- Visible **MARK HIT** control during live sessions.
- Optional user label / perceived phrase prompt.
- Hit records store user-perceived text only.
- Hit JSON and JSONL records explicitly set:
  - `generated_words=false`
  - `word_bank=false`
  - `tts=false`
- Blind review bundle export includes:
  - `blind_review_clip_*.wav`
  - `*_context.wav`
  - per-hit JSON
  - `fieldvox-hit-events.jsonl`
  - `HIT_REVIEW_INSTRUCTIONS.txt`
  - `TRUTH_DO_NOT_SHOW_LISTENERS.tsv`

### Native C source provenance audit

FieldVox now includes native C/JNI audit code in `android/app/src/main/cpp/fv_source_audit.c`.

The native audit writer creates:

- `source_audit_*.json`
- `fieldvox-source-audit.jsonl`
- `SOURCE_AUDIT_README.txt`

Each source-audit record wraps the original hit JSON as `source_hit_json` and records native metadata such as:

- `native_writer="c_jni"`
- `implementation_language="C"`
- binary magic marker
- hit JSON hash
- `network_access_from_native=false`
- `generated_words=false`
- `word_bank=false`
- `tts=false`

### Native C timeline ring buffer

Stage 1O-B added a native C ring buffer for recent event timeline state. Each native source-audit export can include:

- `timeline_window_ms=60000`
- `timeline_snapshot[]`
- timeline event kinds such as ADB test requests, marked-hit events, and native sweep events

This makes hit review easier because the export can show what the app believed was happening immediately before and during the marked hit.

### Native C deterministic sweep scheduler audit

Stage 1P-A added an evidence-grade deterministic scheduler layer. This stage records schedule evidence only; it does **not** change the audio playback engine yet.

New native sweep export files:

- `sweep_schedule.jsonl`
- `sweep_manifest.json`
- `source_slots.json`

Each hit source audit can include a `sweep_engine` block with:

- scheduler: `native_c_deterministic`
- deterministic seed
- active slot
- dwell and jitter timing
- schedule sequence number
- overlap policy: `single_open_slot`
- station/source slot identity
- stream URL hash placeholder
- `audio_engine_changed=false` in the manifest

The current source-slot labels are limited to clean source names used by FieldVox Lab, including KIRO Newsradio Seattle 97.3, Northwest Newsradio KOMO/KNWN, KUOW, and BBC.

### Clean-room and safety position

FieldVox Lab does not use proprietary app branding, assets, databases, or marketing copy from any commercial paranormal app. It does not include a haunted-place database. It is an experimental audio/session logging tool, not a claim generator.

The app should be described as:

- experimental
- clean-room
- open-source
- user-labeled
- evidence-audit oriented
- not proof of paranormal communication

Avoid describing hit labels as confirmed words or messages. They are user-perceived labels recorded for later blind review.
<!-- FIELDVOX_CURRENT_FEATURES_END -->

