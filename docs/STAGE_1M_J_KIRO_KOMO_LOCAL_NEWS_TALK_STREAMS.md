# Stage 1M-J: KIRO/KOMO local news/talk stream bank

FieldVox Stage 1M-J keeps the phonetic-grain VOX engine and adds Seattle-local spoken-word streams near the front of the online bank.

## Added local spoken-word sources

- KIRO Newsradio Seattle 97.3.
- Northwest Newsradio KOMO/KNWN.

## Source-bank rule

The active bank should remain spoken-word focused:

- Local news.
- Public affairs.
- Talk-heavy public radio.
- World news relay.
- Marine and weather spoken-information fallback.

## Clean-room behavior

- Uses real online stream audio as source material.
- Sensor signatures open short phonetic-sized grain packets.
- Supports magnetometer, gyroscope, proximity-near, proximity-far, startup, and auto-scan gate reasons.
- Keeps timeout-safe stream preparation so slow or dead sources are skipped.
- Keeps microphone WAV/session recording active for evidence review.

## Explicit non-goals

- No word bank.
- No text-to-speech.
- No generated words.
- No proprietary third-party app code, branding, database, UI assets, or marketing copy.
