# FieldVox Lab Coding Standards

Judgement-call conventions enforced during `/code-review` (deterministic checks live in `scripts/check.sh`).

- **C23 standard:** Keep first-party C code valid ISO C23 (`-std=c23`, `#if __STDC_VERSION__ < 202311L`) with zero compiler warnings. Wrap third-party headers rather than modifying vendored code if they emit noise.
- **UI layout separation:** Compute widget positions, drawer visibility masking, and bounds inside `fv_ui_layout.[ch]` (`WidgetRect rects[W_COUNT]`), keeping `fieldvox_native_activity.c` focused on rendering already-placed rects and dispatch.
- **Shared primitives over inline duplicates:** Reuse `fv_jni_helpers.h` for JNI lookups/exception checks and `fv_wav_build_header` in `fv_recording_data.h` for RIFF/WAVE headers instead of inlining boilerplate in callers.
- **Minimal public headers:** Keep internal byte-packing or row-layout helpers `static` in `.c` files unless called across translation units; avoid speculative output structs that duplicate `rects[W_COUNT]`.
- **Hygiene:** Never commit local usernames, email addresses, `/home/<user>` paths, or untracked build artifacts.
