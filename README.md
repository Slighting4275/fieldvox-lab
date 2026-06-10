# FieldVox Lab

FieldVox Lab is an experimental EVP/ITC audio review app. EVP means Electronic Voice Phenomena: audio recordings that people later review for words, voices, or phrases they believe they heard. ITC means Instrumental Transcommunication: using electronic tools such as audio recorders, radios, live audio streams, sensors, or computers as part of a session.

FieldVox helps you record sessions, mark interesting moments, and export audio and notes for review. It does not prove that any sound is paranormal, that any voice is real, or that communication happened. It gives you organized recordings, hit markers, review clips, and logs so you can study what was captured more carefully.

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
## What FieldVox Lab is

FieldVox Lab is an experimental Android app for EVP/ITC audio sessions and for keeping a clear record of what happened during those sessions.

A simple way to think about it:

- The app listens and records audio.
- It watches sensor and session state while recording.
- You can press **MARK HIT** when you hear something interesting.
- The app saves the audio, the session log, and the hit marker together.
- The app can export review files so someone can listen first and read labels later.

FieldVox Lab is **not proof of paranormal communication**. It does not prove that a word, phrase, voice, or message came from any outside source. It is a logging and review tool. Any hit label is a user-perceived label, not a confirmed message.

Current verified Codeberg checkpoint: `e6045f8` / `e6045f876d3d1980709358efd05efe38235ccaa7`
Latest commit subject: `Document Stage 1P-B canonical source catalog`

## What a non-technical person should know

### What EVP and ITC mean

EVP means Electronic Voice Phenomena. In plain language, it means recording audio and later reviewing it for words, voices, or phrases that someone believes they heard.

ITC means Instrumental Transcommunication. In plain language, it means using electronic tools such as recorders, radios, live audio streams, sensors, or computers as part of a session.

FieldVox Lab supports EVP/ITC-style review, but it does not prove that a sound is paranormal, that a voice is real, or that communication happened.

### What happens when you run a session

When you start a session, FieldVox Lab records the session audio and keeps a matching event log. The audio is saved as a WAV file. The log is saved as a JSONL file, which is just a line-by-line text log of events.

For example, one session may create:

- `session-123.wav` — the sound recording
- `session-123.jsonl` — the matching event log

Those two files belong together. The WAV is what you listen to. The JSONL is the timeline of what the app recorded about the session.

### What MARK HIT means

If you hear something interesting, you can press **MARK HIT**. The app records the time of that hit and lets you add an optional label for what you think you heard.

The important rule is this: FieldVox records **what you typed or marked**. It does not generate words for you.

Hit records explicitly say:

- `generated_words=false`
- `word_bank=false`
- `tts=false`

That means the app is not using a word bank, not using text-to-speech, and not generating phrases.

### What blind review export means

A blind review export is meant to make review more fair.

The app can export audio clips with neutral names, plus a separate truth file. The idea is that a listener can hear the clip first without being told what the expected phrase is. After listening, the reviewer can compare their notes against the truth file.

Exported review files can include:

- blind review WAV clips
- context WAV clips from around the hit
- hit JSON files
- `fieldvox-hit-events.jsonl`
- `HIT_REVIEW_INSTRUCTIONS.txt`
- `TRUTH_DO_NOT_SHOW_LISTENERS.tsv`

### What Export Manifest means

Export Manifest creates a simple table-of-contents file named:

- `fieldvox-session-manifest.tsv`

This file lists complete session pairs where the app found both:

- a session JSONL log
- a matching session WAV recording

That makes it easier to know which audio file belongs with which log file. It is like a checklist for exported sessions.

The manifest is exported to:

- `Downloads/FieldVoxLab`

That makes it easier to find from Android Files, copy to a computer, or share for review.

### What source audit means

A source audit is extra proof-of-process information. It records what the app knew about the session, hit, timeline, and source metadata when a hit was marked.

It does not prove that a hit is paranormal. It helps reviewers see what the app did and what files were created.

## For technical people: what the app does

### Android app identity

- Package: `org.fieldvox.lab`
- Main activity: `org.fieldvox.lab/.MainActivity`
- Main Java source: `android/app/src/main/java/org/fieldvox/lab/MainActivity.java`
- Native C/JNI audit source: `android/app/src/main/cpp/fv_source_audit.c`

### Storage model

FieldVox Lab keeps working session files in app-private storage and exports review files to public Downloads through the app export flow.

App-private working data includes:

- session WAV files
- session JSONL files
- hit JSON files
- native source-audit JSON files
- native scheduler/audit JSON and JSONL files

Public exported files go under:

- `Downloads/FieldVoxLab`

The app is designed to avoid broad storage permissions. Exported public files are user-facing review artifacts.

### Session files

A normal session can produce matched file pairs:

- `session-<id>.wav`
- `session-<id>.jsonl`

The WAV file contains recorded audio. The JSONL file contains session events, sensor/gate metadata, source metadata, hit marker events, and audit-related records.

The export manifest file:

- `fieldvox-session-manifest.tsv`

lists matched JSONL/WAV session pairs so reviewers can connect each audio recording to its matching event log.

### Hit marker records

When the user marks a hit, the app creates structured hit records with fields such as:

- `event_type=hit_marker`
- hit ID
- session ID
- hit offset/time
- optional `user_label`
- optional `perceived_phrase`
- source label/source URL metadata when available
- gate reason and packet metadata
- sensor/gate state
- `generated_words=false`
- `word_bank=false`
- `tts=false`

The label is user-perceived. The app should not describe labels as confirmed words or confirmed messages.

### Native C source provenance audit

The app includes native C/JNI audit writing in:

- `android/app/src/main/cpp/fv_source_audit.c`

Native audit exports include:

- `source_audit_*.json`
- `fieldvox-source-audit.jsonl`
- `SOURCE_AUDIT_README.txt`

Each source-audit record can include:

- `native_writer="c_jni"`
- `implementation_language="C"`
- binary magic marker
- hit JSON hash
- `source_hit_json`
- `network_access_from_native=false`
- `generated_words=false`
- `word_bank=false`
- `tts=false`

The native audit writer does not open network connections and does not generate text.

### Native C timeline ring buffer

Stage 1O-B added a native C ring buffer for recent event timeline state.

Source-audit records can include:

- `timeline_window_ms=60000`
- `timeline_snapshot[]`

The timeline can contain recent event kinds such as:

- ADB test requests
- marked-hit events
- native sweep events
- carrier/source state events when recorded

This helps reviewers understand what the app believed was happening around the time of a hit.

### Native C deterministic sweep scheduler audit

Stage 1P-A added a deterministic native scheduler evidence layer. This records scheduler evidence. It does not change audio playback yet.

Native sweep files include:

- `sweep_schedule.jsonl`
- `sweep_manifest.json`
- `source_slots.json`

A hit source audit can include a `sweep_engine` block with:

- scheduler: `native_c_deterministic`
- deterministic seed
- active slot
- dwell timing
- jitter timing
- schedule sequence
- overlap policy: `single_open_slot`
- source slot identity
- stream URL hash placeholder

### Stage 1P-B source-catalog policy and canonical JSON

Stage 1P-B hardens source-slot metadata so it is ready for future Radio Browser style station discovery.

The exported policy fields include:

- `catalog_provider=manual_seed_v1_radio_browser_compatible`
- `radio_browser_compatible=true`
- `uuid_policy=stationuuid_required`
- `countrycode_policy=countrycode_required`
- `server_discovery_policy=future_dns_all_api_radio_browser_info_randomize_retry`
- `http_user_agent_policy=speaking_agent_required`
- `raw_stream_urls_exported=false`

The canonical Stage 1P-B commit removed duplicate JSON object names from `sweep_manifest.json` and validated both app-private and exported JSON with a duplicate-key detector.

Playback remains unchanged in Stage 1P-B.

## Clean-room and safety position

FieldVox Lab is a clean-room open-source experimental tool. It does not use proprietary branding, assets, databases, or marketing copy from any commercial paranormal app. It does not include a haunted-place database.

Use careful language:

- say “user-perceived hit”
- say “marked hit”
- say “review clip”
- say “session log”
- do not say the app proves communication
- do not say the app confirms words or messages

FieldVox Lab should not be used for health, safety, legal, financial, grief, or other important life decisions.
<!-- FIELDVOX_CURRENT_FEATURES_END -->
