# Stage 1M-H: phonetic-grain stream synthesis

FieldVox Stage 1M-H adds a clean-room granular VOX layer.

## What it does

- Uses real online radio stream audio as source material.
- Uses sensor signatures to open short packets of phonetic-sized audio grains.
- Supports magnetometer, gyroscope, proximity-near/proximity-far, startup, and auto-scan gate reasons.
- Keeps stream prepare timeouts so slow streams do not stall queued proximity events.
- Keeps microphone WAV/session recording active for evidence review.

## What it does not do

- No word bank.
- No text-to-speech.
- No generated words.
- No proprietary third-party code, branding, database, UI assets, or marketing copy.

## Evidence handling

Every audible response is still a real stream snippet. Treat perceived phrases as candidate hits only. Stronger review requires blind listening, saved clip evidence, exact station/source logging, timestamp logging, and independent listener agreement.
