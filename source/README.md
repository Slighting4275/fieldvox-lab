# FieldVox Lab host tools

This directory contains the C23 host simulator, station catalog generator, recording utilities, and regression tests for the Android application. See the [main README](../README.md) for the current Android features, controls, build instructions, and storage layout.

## Build and test

From the repository root:

```bash
make -C source test
```

The suite builds and runs:

- Native unit tests for sensor parameters, control helpers, audio DSP, voice slots, hit capture, and cache eviction.
- Recording tests for WAV parsing, waveform data, seeking, and bookmarks.
- Fragment-engine tests for independent pitch and speed, four finite echo repeats, voice capacity, speech holdoff, cooldowns, cache reuse limits, and persistent offline clips.
- Simulator smoke tests for deterministic output, CSV input, JSONL logs, WAV readability, binary blob extraction, malformed inputs, escaped paths, output collisions, and truncated or non-finite payloads.

The `fieldvox` simulator models magnetometer data and writes temporary PCM blobs, rendered WAV audio, and JSONL logs. It is a host-side test tool; the Android app uses real sensors and broadcast or selected-WAV audio.

## Utilities

| Executable | Purpose |
| --- | --- |
| `fieldvox` | Run the deterministic sensor/audio simulation. |
| `fv-csvvox` | Process sensor readings from CSV. |
| `fv-blobdump` | Inspect or extract simulator audio blobs. |
| `fv-station-blob-gen` | Generate the embedded station plan from the C23 catalog. |

Run `make -C source blobgen` to build and execute the station generator. Host tests verify portable behavior; Android camera, codec, and lifecycle behavior still needs device testing.

FieldVox does not claim that sensor readings or audio fragments demonstrate paranormal communication.
