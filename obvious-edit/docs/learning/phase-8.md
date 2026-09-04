# Phase 8 — Export and the render seam

*The guided walkthrough for the H.264/AAC export phase: what was built,
why it is shaped that way, and what the tests actually prove.*

## 1. Phase purpose

Everything before Phase 8 lives in the editor: frames appear in the
program monitor, audio plays through SDL, and none of it exists outside
the running process. Phase 8 closes the loop — **File › Export… turns
the timeline into an MP4** — and does it under one constraint that
shapes every file in the phase:

> **Preview and export share one render path.** The same GTK-free seam
> that repaints the monitor samples the exported file. A straight cut
> that looks right on screen cannot silently render differently in the
> MP4, because there is no second path to drift.

That rule produces the two new media modules: `oe_render` (the seam —
map a position to the topmost covering clip, decode, composite) and
`oe_export` (the encoder — pump the seam through an H.264/AAC MP4
writer with project-save-style atomicity). Around them: the sequence
gains a resolution (1920×1080 default, persisted additively), the
command registry gains `OE_CMD_EXPORT`, the window gains the chooser
and progress dialog plus a worker thread, and the test suite grows to
sixteen.

The self-check contract held: zero-warning `-Werror` build, 16/16
plain suites, 16/16 under ASan/UBSan, 16/16 under Valgrind with the
existing suppression file (no new suppressions), full-tree
clang-format, and `scripts/run-headless.sh` exit 0 — plus dogfooded
export runs recorded headlessly on Xvfb.

## 2. Per-file explanations

| File | Role in this phase |
|---|---|
| `src/media/oe_render.[ch]` | The frame-at-time seam. `OeRenderSource` binds a sequence to two GTK-free callbacks (media-ref → owned path, plus the media cache); `oe_render_frame_at` returns a freshly owned opaque BGRA canvas. The session variant caches one sequential decoder per source path. |
| `src/media/oe_export.[ch]` | The synchronous export job. Integer frame grid over the render seam, libx264 H.264 + native AAC into MP4, additive 48 kHz stereo mixdown, custom AVIO over a temp fd, atomic rename finalize, typed `OeExportError`s including `CANCELLED`. |
| `src/core/oe_project.c` | `OeSequence` gains `width`/`height` (default 1920×1080 via `OE_SEQUENCE_DEFAULT_*`), deep-copied with the sequence; `oe_project_set_sequence_size` validates positive + even and notifies observers. |
| `src/core/oe_project_format.c` | `width`/`height` join the strict known-member list and are always written; on read they are optional — absent fields backfill to defaults, wrong types report `TYPE`, invalid values report `VALUE`. |
| `src/app/oe_command.c/.h` | `OE_CMD_EXPORT` appended **before** `OE_CMD_COUNT` — the stable-index rule that keeps registry table order tests honest. |
| `src/ui/oe_main_window.c` | The chooser (MP4-filtered `GtkFileDialog`, `<title>.mp4` initial name), the modal progress window (bar + Cancel), the window-owned `GThread`, `g_main_context_invoke` marshalling, the session-epoch staleness tag, and the atomic cancel flag. |
| `tests/test_export.c` | The 16th suite: eight tests driving `oe_export_run` with no UI at all, over synthetic in-process fixtures (≤ 160×120, ~25 frames). |
| `tests/fixture_media.[ch]` | Two generators join the runtime fixtures: solid-color MJPEG AVI clips and constant-amplitude PCM WAV clips. |

## 3. Block-by-block build walkthrough

`meson.build`, in order:

1. **Core first** — the model changes compile in the existing
   `oe_project`/`oe_project_format` sources; no new target. New tests
   extend the existing suites.
2. **`oe_render`** — added to the media sources. It links only FFmpeg
   + GLib: no GTK, no SDL. The test target pulls it together with
   `oe_playback_session.c` (for mapping parity) and the SDL adapter
   (for the playback-session link surface).
3. **`oe_export`** — depends on `oe_render` and nothing else in the
   app. The dependency direction is the point: export cannot reach
   around the seam.
4. **The UI** — the window compiles the chooser/progress/worker code;
   `OE_CMD_EXPORT` slots into the registry before `OE_CMD_COUNT`.
5. **`test_export` executable + `test()`** — registered with the
   media-jobs-style environment; the suite name is `export`.

The verification battery, in the order it runs in CI:

```
ninja -C build                                    # zero warnings, werror
meson test -C build                               # 16/16
meson test -C build-san                           # 16/16 ASan/UBSan
meson test -C build --wrapper "valgrind --tool=memcheck \
  --leak-check=full --error-exitcode=1 \
  --suppressions=$(pwd)/tests/valgrind.supp"      # 16/16
clang-format --dry-run --Werror src/*.c src/*/*.c src/*/*.h tests/*.c
./scripts/run-headless.sh                         # exit 0
```

## 4. C concepts in play

**Caller-owned buffer contracts.** `avio_alloc_context` does not take
ownership of the buffer you hand it: `avio_context_free` frees the
context, and the buffer stays the caller's — freed by capturing
`avio->buffer` *before* freeing the context (the AVIO layer may have
grown it). Getting this backwards leaks 64 KiB per export or double-
frees; see bug log item 2.

**Drain loops.** FFmpeg encoders and decoders both speak the
send/receive protocol: feed input until `EAGAIN`, drain output until
`EAGAIN`, then send `NULL` and drain to `AVERROR_EOF`. The export
video loop, the AAC audio encoder, and the test decode-back helpers
are all the same shape.

**Atomic cross-thread flags.** The cancel flag is a plain `gint`
touched only through `g_atomic_int_*` — set by the main thread,
read by the worker between frames. One-directional, no state machine,
no lock.

**Additive schema change.** Optional members + backfill = no version
bump: a Phase 7 file cannot contain the new fields, and a current
reader normalizes absence to defaults before the model exists. The
strict reader keeps rejecting everything else.

## 5. Ownership table

| Object | Allocated by | Owned by | Freed by |
|---|---|---|---|
| Rendered BGRA canvas | `oe_render_*_frame_at` | the caller, immediately | caller's `g_free` |
| Per-source decoder (session) | first `frame_at` for a path | the `OeRenderSession` | `oe_render_session_free` |
| Held last frame | `ensure_source` | the session (video state) | `oe_render_session_free` |
| AVIO buffer (64 KiB) | `oe_export_run` | the caller, always | `av_free` after `avio_context_free` |
| Custom AVIO context | `avio_alloc_context` | the job | `avio_context_free` |
| Temp file fd | `g_mkstemp` | the job | `g_close` on every path |
| Encoder/muxer contexts | job setup | the job | `export_encoder_free` / `avformat_free_context` |
| Sequence snapshot + ref→path map | window (`export_start`) | the worker closure | worker after `oe_export_run` |
| Completion payload | worker | the main-context callback | callback after dispatch |
| Progress dialog | window | the window | closes on done/cancel/fail |

The two ownership bugs in the bug log are both rows of this table that
were first written wrong.

## 6. Call flow

```
[main thread]                          [worker thread]
chooser → accepted(path)
  snapshot sequence (deep copy)
  snapshot ref→path map
  progress dialog + cancel flag
  g_thread_new ───────────────────────► oe_export_run (spec)
                                          for f in 0 .. grid-1:
                                            cancel? → CANCELLED
                                            frame f → render session
                                              → sws BGRA→YUV420P
                                              → x264 send/recv
                                          mixdown: WAV decode all
                                            tracks → swr 48k f32 → sum
                                          av_write_trailer
                                          fsync(temp)
                                          rename(temp → path)
  g_main_context_invoke ◄────────────────  done: {error, path, epoch}
  epoch matches? dialog closes
  status bar: "Exported <path>"
```

The test path skips the left column entirely: `test_export.c` builds a
spec, points it at a temp destination, and calls `oe_export_run` on
the main thread — the job is UI-free by construction.

## 7. Alternatives considered

- **Per-frame backward seeks, reusing `decode_at`** (the playback
  decoder's public habit). Rejected on the research artifact's
  measured cost: a backward seek per frame turns a 25-frame export
  into 25 re-decodes of the same streams and an O(n²) tail. The
  session's sequential decoder exists exactly for this.
- **libx264 by name only.** Rejected: `avcodec_find_encoder_by_name`
  hard-depends on the x264 symbols being compiled into the distro's
  FFmpeg. The by-ID fallback (`AV_CODEC_ID_H264`) keeps the encoder
  native-and-real everywhere; yuv420p and the frame grid do not
  change.
- **A `GTask` async export.** Rejected as a second scheduling layer:
  the window already owns a dedicated worker thread and marshals with
  `g_main_context_invoke` (the import-worker pattern). GTask would
  hide the thread instead of documenting it.
- **Writing the MP4 to a system temp dir.** Rejected: `g_rename` is
  only atomic within one filesystem. The temp lives in the target
  directory (the `oe_project_format.c` pattern), so finalize is a
  same-device rename.
- **Absolute amplitude assertions for the audio tests.** Rejected
  after measurement: AAC's psychoacoustic filter attenuates pure DC
  by ~30%, so "0.25 in must be ~0.25 out" fails while the audio is
  genuinely fine. The mixdown proof is a ratio (overlap ≥ 1.4× either
  single track) — structural, not bit-exact.
- **Suppressing the tail-frame decode quirk.** Rejected: it is not a
  leak (a suppression would be a lie in a policy file scoped to
  GLib/GObject internals). The container truth test asserts all 25
  frames exist; the round-trip decodes the last *decodable* frame.

## 8. Bug log

1. **NULL deref in `deliver` (`av_frame_ref` on `vs->held` before
   allocation).** The held-last-frame storage was used before it was
   allocated. ASan caught it on the first sanitizer run. Fix:
   allocate in `ensure_source`, where every other per-source resource
   is born. Lesson: new struct fields get their allocation where the
   struct is born, not at first convenient use.
2. **AVIO buffer ownership, misread twice.** First pass: freed nothing
   (64 KiB leaked per export — LSan). Second pass: guessed the
   contract was "ownership transfers to the context," which produced
   a double-free under ASan. The real contract (FFmpeg docs + two ASan
   runs): `avio_context_free` never frees a caller-provided buffer;
   capture `avio->buffer`, free the context, then `av_free` the
   buffer. Lesson: in C, "who frees this" is a documented contract,
   not a guess — read the header comment before the second attempt,
   not after the third crash.
3. **The missing 25th frame (decoder-side quirk).** The exported MP4
   holds all 25 frames (`nb_frames=25`, duration exactly on the grid),
   but this FFmpeg build's h264 decoder emits only 24: the final
   access unit of an x264 stream whose closing GOP was shortened by a
   scenecut is never presented. A standalone decoder (single-threaded,
   with `pkt_timebase`, with post-flush EAGAIN spinning) reproduced it
   in every configuration, so the test decodes the last *decodable*
   frame and asserts its color class. Logged as a known environment
   limitation, not a defect in the export.
4. **AAC attenuates DC.** The first mixdown test asserted absolute
   levels derived from the source WAV; AAC's transform pipeline reads
   0.25 DC back at ~0.177. The assertion became a ratio-based
   summation proof. Lesson: lossy codecs are for structure, not
   amplitudes — test classes, not levels.
5. **`oe_sequence_init` before `oe_project_get_sequence` (test
   misuse).** `get_sequence` hands over a whole fresh copy into zeroed
   caller storage; pre-initializing leaks the fresh `tracks` array
   (exactly 32 bytes × 8 tests, one per init). Fix: zeroed storage at
   all three call sites. The same init-then-overwrite shape exists in
   `oe_timeline.c`'s refresh path (pre-existing, flagged out of scope
   for this phase).

## 9. What is next

Phase 8 leaves the editor with a complete vertical: import → edit →
play → **export**. What later work inherits from this phase:

- **The seam is the extension point.** Effects and transitions are
  edits to the compositing step inside `oe_render_frame_at` — per-clip
  filters compose before the box-fit; a transition samples two
  positions and blends. No export code should need to change.
- **The grid is the timing contract.** Frame-accurate features (audio
  sync patches, subtitle burn-in) anchor to `oe_export_frame_to_us`.
- **The 16-suite pattern.** Each phase's suite doubles as the next
  phase's fixture source; `fixture_media` grew encoded-video fixtures
  here and they are reusable.

## 10. Five review questions (with answers)

1. **Why does export go through the render seam instead of sampling
   the monitor's frame queue?** The monitor renders on demand at
   wall-clock speed with GTK ownership of the frame; export needs
   deterministic sampling at grid positions on a worker thread. Both
   call `oe_render_frame_at` — parity by construction, and the
   parity test holds them to it (same mapping semantics, dominant
   color either side of a cut).
2. **Why is the export synchronous if the UI shows a progress
   dialog?** Synchronous ≠ blocking: the *job* is synchronous (one
   call, cancel flag, progress callback — the media-jobs convention);
   the *window* runs it on its own thread. This keeps the job
   testable without GTK and the UI free of job internals. The
   progress/cancel plumbing is the import worker's, already reviewed
   in Phase 2's pattern.
3. **How does cancellation stay race-free without a mutex?** The flag
   is one-directional: main thread sets it, worker only reads it, all
   access is atomic. The worker checks between frames; the worst
   latency is one frame of encode. There is no state to lock because
   nothing writes back — the result payload reports what happened.
4. **Why is the temp file created in the destination's directory?**
   `g_rename` is atomic only within a filesystem; a temp in `/tmp`
   would make finalize a copy across devices (non-atomic, and a
   partial file at the destination on failure). The destination-dir
   temp + fsync + rename gives the same guarantee as project saves:
   the destination is either the old file or the complete new one.
5. **Why backfill 1920×1080 for missing `width`/`height` instead of
   rejecting old files?** The fields are additive: a Phase 7 file
   cannot contain them, so absence carries no information loss — the
   defaults are exactly what those projects rendered at. Rejecting
   them would orphan every existing project for zero benefit, and a
   version bump for a strictly-additive change would break the
   format's promise that versions mark *incompatible* change.
