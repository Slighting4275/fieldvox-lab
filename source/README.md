# FieldVox Lab

Clean-room open-source environmental-sensor-triggered audio experiment.

This first stage is a Linux C simulator:

- simulates magnetometer readings;
- detects magnetic-field fluctuation signatures;
- generates temporary PCM audio binary blobs;
- renders triggered audio to WAV;
- writes JSONL logs for auditability.

This project does not claim to detect ghosts, prove paranormal activity, or communicate with spirits.

Build and run the deterministic smoke tests with:

    make test

The tests validate deterministic simulator output, CSV ingestion, JSONL syntax, WAV readability,
blob extraction, malformed arguments, escaped paths, and rejection of truncated blob payloads.
