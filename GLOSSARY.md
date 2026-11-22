# FieldVox Lab Domain Glossary

- **Trigger**: A sensor reading or auto-scan timer event that passes decimal or threshold admission to open a fragment window.
- **Fragment**: A pitch- and tempo-transformed audio grain (typically 400–760 ms) sliced from a fetched radio clip or local WAV bank.
- **Speaker Adapter**: Output sink that receives complete source audio or descriptors (Android `MediaPlayer` + `EnvironmentalReverb`, or a host test fake).
- **Clip Source**: Bounded on-demand radio stream fetcher and decoder delivering 48 kHz mono PCM and a compressed source file descriptor.
- **Clip Store**: Persistent offline disk cache holding decoded fragment WAVs paired with compressed `.wav.audio` originals.
- **Hit Capture**: A paired 3-second pre-roll clean/processed WAV recording and timeline JSON saved when the operator marks a hit.
- **UI Controller**: Stateful model owning screen mode, overlay visibility, drawer masking, widget geometry, tap dispatch, and slider domain scaling.
