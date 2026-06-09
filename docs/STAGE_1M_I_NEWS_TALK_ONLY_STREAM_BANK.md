# Stage 1M-I: news/talk-only stream bank

FieldVox Stage 1M-I introduced the phonetic-grain VOX engine but restricts the online stream bank to spoken-word sources.

## Included source types

- News radio.
- Public affairs.
- Talk-heavy public radio.
- World news relay.
- Marine and weather spoken-information fallback.

## Excluded source types

- Non-speech entertainment channels.
- Performance-format channels.
- Instrument-first channels.
- General entertainment discovery streams.

## Clean-room behavior

- Uses real online stream audio as source material.
- Sensor signatures open short phonetic-sized grain packets.
- Supports magnetometer, gyroscope, proximity-near, proximity-far, startup, and auto-scan gate reasons.
- Keeps timeout-safe stream preparation.
- Keeps microphone WAV/session recording active for evidence review.

## Explicit non-goals

- No word bank.
- No text-to-speech.
- No generated words.
- No proprietary third-party app code, branding, database, UI assets, or marketing copy.


## Stage 1M-J follow-up

Stage 1M-J adds Seattle-local spoken-word sources near the front of the bank: KIRO Newsradio and Northwest Newsradio KOMO/KNWN.
