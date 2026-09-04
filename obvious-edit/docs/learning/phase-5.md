# Phase 5 — the playback clock: time becomes real

A guided walkthrough of Phase 5: what was built, why it is shaped this
way, and what to look at when you read the code.

## 1. Phase purpose

Phase 4 made the model visible and touchable, but the playhead was a
view artifact — a number the widget kept, advanced by inserts. Phase 5
gives it meaning: a playback session owns time, the monitor shows
frames, and the transport obeys the project.

Concretely, five things appear:

- `src/app/oe_playback_session.[ch]` — the GTK-free clock: a
  stopped/paused/playing state machine over a project snapshot, a
  wall-clock-anchored position in integer microseconds, `tick()`
  returning the next frame deadline in monotonic µs, and pure
  clip→source mapping. It links GLib, FFmpeg, and SDL — never GTK.
- `src/media/oe_media_playback.[ch]` — full-resolution decode: an
  audio decode-ahead worker (GThread + GAsyncQueue, main-context
  delivery) producing owned interleaved f32 chunks, and frame-at-time
  video decode to packed BGRA at monitor resolution.
- `src/playback/oe_audio_output.[ch]` (extended) — the SDL3
  push-model device stream on the Phase 0 adapter: open once, queue
  interleaved f32, report depth, flush, pause/resume, close.
- `src/ui/oe_program_monitor.[ch]` — the program monitor: a
  GtkDrawingArea that blits owned frames, keeps the empty state until
  the first frame, and hatches missing media.
- Wiring — the window replaces the Program Monitor placeholder,
  schedules one GSource at the session's deadline, routes
  ruler-drag seeks into the session through the timeline's new
  playhead-changed callback, and installs `transport.play-pause` and
  `transport.stop`.

What Phase 5 deliberately does **not** add: shuttle/multi-speed and
reverse (multi-speed needs a rate knob on the clock — the owning-phase
comments now say Phase 6+), mark-in/mark-out range playback, looping,
source-monitor playback, and sample-accurate A/V sync — the honest
limits of a dummy-driver CI environment.

## 2. Per-file explanations

| File | Role |
|---|---|
| `src/app/oe_playback_session.[ch]` | The GTK-free state machine: anchor + state + project snapshot; `play/pause/stop/seek/tick`; pure `oe_playback_session_map`; typed events (nothing-to-play, end-of-sequence, missing-media-skipped); observer notify on the main context. |
| `src/media/oe_media_playback.[ch]` | Decode layer: audio worker thread with generation-token requests (a seek supersedes the in-flight request instead of queueing behind it), owned f32 chunks delivered via `g_main_context_invoke`; video decodes frame-at-time to packed BGRA, box-fitted to the monitor size. |
| `src/playback/oe_audio_output.[ch]` (extended) | Push-model device stream on the existing adapter: `open_stream` (device-native rate/channels as f32, never resampled), `queue`, `queued_frames`, `flush`, `set_running`, `close_stream`; `is_dummy` reports SDL's dummy driver. |
| `src/ui/oe_program_monitor.[ch]` | `OeProgramMonitor` over `GtkDrawingArea`: owns delivered frames, zero-copy Cairo blits (BGRA ↔ ARGB32 compatible byte order), empty state, missing-media hatch. |
| `src/ui/oe_timeline.[ch]` (extended) | Playhead-changed callback mirroring the resolve/report seams; programmatic `set_playhead` never re-fires it (no loops). |
| `src/ui/oe_main_window.c` (changed) | Session lifecycle (create, feed, teardown before the project), tick GSource, transport handlers with status-bar reports, session recreation on project replace, monitor + observer wiring. |
| `src/app/oe_command.h` (comment fix) | Corrected owning-phase notes: shuttle and marks are Phase 6+, the keymap note now matches the real table. |
| `tests/test_playback_clock.c` | Ten headless cases: mapping (topmost, half-open, clamping), deadlines, empty sequence, end-of-sequence, pause/resume drift, stop parking, seek clamping and live seek, missing-media reporting. |
| `tests/test_audio_output.c` | Seven adapter-contract cases under SDL's dummy driver: init edges, open reporting, queue depth, flush, pause/resume preservation, NULL tolerance. |
| `meson.build` | New sources in the executable list; two new test targets (13 suites total) with `SDL_AUDIODRIVER=dummy`. |
| `docs/*` | Architecture "The playback clock (Phase 5)" section, code-map rows, glossary Clock / A/V sync / Frame pacing, this file. |

## 3. Block-by-block build walkthrough

**The clock is an anchor, not an accumulator.** Playing stores
`anchor_position_us` and the monotonic time it corresponds to; every
`get_position` recomputes `anchor + now - anchor_wall`. Pause freezes
the position and remembers it; resume re-anchors there. Seek just
re-anchors. Nothing integrates dt anywhere, so nothing can drift: a
slow draw or a busy main loop makes positions *late*, never wrong.

**Deadlines are frame-anchored.** `tick()` computes the position,
pushes it, and returns the next deadline one frame interval ahead,
computed from the sequence rate as a rational (25 fps = exactly
40 000 µs). The window schedules its single GSource at that deadline —
when a draw runs long, the next tick lands on the next deadline, not
a fixed 40 ms after the late one.

**Mapping is pure and total.** `oe_playback_session_map(sequence,
kind, position, &out)` walks tracks from the topmost (highest index)
down, answers with the owning clip and source microsecond (clamped
into `[source_in, source_out)`), or reports "nothing". The session
uses it for audio and video; the tests use it directly. Half-open
spans mean the frame at clip end belongs to whatever is next — or
nothing.

**End-of-sequence is a fact of the snapshot.** The session keeps a
deep copy (`oe_project_get_sequence`); its extent is the stop line.
A trim that lands mid-playback changes when playback stops, because
the next play re-snapshots. When the position reaches the end, the
session stops, parks the playhead exactly at the end, and fires
`END_OF_SEQUENCE` once.

**Audio runs ahead; video keeps up.** The session submits audio
requests spanning position + lookahead, tagged with a generation
counter. Pause/stop/seek bumps the generation: the worker discards
stale work and the session ignores stale deliveries, so a seek never
hears audio from two seconds ago. The worker decodes to interleaved
f32 at the device's native rate and hands over owned chunks on the
main context; the session queues them into SDL and compares queue
depth against wall position to nudge the visible position (drift
snap/slew) when a real device disagrees with the wall clock.

**The adapter is the only SDL the session sees.** `oe_audio_output`
owns every SDL symbol. The no-device path is a typed error and a
supported state: playback continues wall-clock-only, `is_dummy` tells
the session that queue depth is not device truth, and teardown stays
symmetric — close the stream, then shut the subsystem down.

**The monitor never meets a decoder.** Decoded frames are owned
packed-BGRA buffers the monitor takes over (BGRA reuses Cairo's
ARGB32 byte order, so a blit needs no per-frame channel swap — one
swsCOLOR written once instead of three bytes per pixel every frame).
Empty until the first frame, hatched when media is missing, box-fitted
always. The draw function touches no FFmpeg type.

**Wiring stays thin.** The window creates the session from the
borrowed project, recreates it when the project is replaced, fires
`tick()` from one GSource, and translates: timeline drag → seek;
session position → playhead; session events → status bar. Space
toggles play/pause, S stops with the playhead parked; an empty
sequence reports "nothing to play" without an error dialog.

## 4. C concepts in play

- **Wall-clock anchoring.** Store (position, timestamp) pairs, never
  integrate velocity. This is the same integer-µs discipline as
  `oe_time`, extended from single instants to moving positions.
- **Generation tokens.** Monotonic counters turn "cancel the pending
  work" from a pointer-chasing exercise into an integer compare — the
  worker-side check is two loads, and stale deliveries fail one test.
- **Main-context discipline.** Every cross-thread handoff lands on
  the main context (`g_main_context_invoke`), exactly as the import
  worker established. UI callbacks need no locks anywhere.
- **Push-model audio.** SDL3's device stream inverts the callback
  model: the session decides when audio exists and pushes it. Queue
  depth becomes a measurable quantity instead of callback folklore.
- **Pixel-format diplomacy.** Choosing the decoder's output format so
  the consumer needs zero conversion (BGRA ↔ Cairo ARGB32) is the
  cheapest performance decision in the phase.
- **Test-order independence.** g_test shuffles; a suite where one
  test's setup leaks into the next is a suite that fails randomly.
  Every Phase 5 test establishes its own state.

## 5. Ownership table

| Object | Owner | Lifetime |
|---|---|---|
| Session (clock, state, snapshot) | the window | recreated on project replace; freed before the project |
| Audio chunks in flight | the worker until delivery, the session after | one decode-ahead window |
| SDL device stream | the adapter; the session closes it | playback session |
| Decoded video frames | the monitor once delivered | until the next frame replaces them |
| Generation counters | the session | per session |
| Sequence snapshot | the session; refreshed on play/seek | between refreshes |
| Playhead position | the session; painted by the timeline | session state, never serialized |

Teardown order (the invariant from Phase 2, now with two threads to
feed): close the SDL stream, drain + join the media worker, free the
session, *then* `oe_ffmpeg_shutdown` and `oe_audio_output_shutdown`.

## 6. Call flow

**Play.** Space → `transport.play-pause` → session `play()` →
refresh the snapshot, open the stream (typed failure = wall-clock
only), submit the first audio request, re-anchor → observer fires
(stopped → playing) → status bar reports.

**Tick.** GSource fires at the deadline → `tick()` → position
computed from the anchor → playhead pushed to the timeline → audio
queued from delivered chunks (queue depth nudges the position if a
real device disagrees) → video mapped, decoded frame-at-time, monitor
blits → next deadline returned.

**Seek during playback.** Ruler drag → playhead-changed callback →
window → session `seek()` → generation bumped, audio queue flushed
(`SDL_ClearAudioStream`), video decoder flushed (`avcodec_flush_buffers`),
clock re-anchored. Playback continues from the new position; stale
worker output is dropped on arrival.

**End of sequence.** Position reaches the snapshot's extent → state
returns to stopped, position parks exactly at the end, one
`END_OF_SEQUENCE` event → status bar reports, transport button resets.

**Missing media.** Decode/open fails → one `MISSING_MEDIA_SKIPPED`
event per run (a per-run failed-path guard stops 25 futile opens per
second) → monitor hatches, status bar reports, the transport keeps
playing the rest of the sequence.

## 7. Alternatives considered

- **Integrating dt per tick.** The obvious clock — `pos += elapsed` —
  accumulates every error: late ticks, pauses, seeks. Anchoring costs
  one extra subtraction per read and makes drift structurally
  impossible. Rejected: accumulator.
- **GLib `GSource` per deadline inside the session.** Keeping the
  session free of the main context (it only *reports* deadlines)
  keeps it testable without a loop — tests call `tick()` themselves.
  Scheduling stays a UI concern.
- **RGBA video frames.** Cairo surfaces are ARGB32 in memory —
  BGRA-named pixels, alpha-first bytes. Decoding to RGBA would swap
  channels on every frame forever; BGRA makes the blit free.
- **One shared decoder thread for audio and video.** Two concerns
  with different pacing (audio needs seconds of lookahead; video
  needs exactly one frame now). Splitting them keeps audio latency
  independent of a slow video frame.
- **Blocking decode on the main thread.** Rejected outright: one
  4K frame stalls playback past two frame intervals. The worker is
  the same pattern the import worker already proved.

## 8. Bug log

- **`oe_project_get_sequence` clobbered my initialization.** The
  deep-copy getter replaces caller storage wholesale and documents
  "must be uninitialized or zeroed" — the session initialized a
  sequence and *then* called it, leaking one `GPtrArray` per
  refresh. LeakSanitizer caught it the moment the ASan gate ran:
  23 blocks, every stack pointing at the same two lines. Fix:
  hand it zeroed storage (`oe_sequence_clear` is a no-op there).
- **Test order dependence.** The first adapter suite assumed
  registration order (`requires-init` before anything that opens a
  stream); it passed locally and failed identically-built in the
  next run because g_test shuffles. Every test now establishes its
  own state; `requires-init` forces *and restores* the state it
  needs.
- **The overlap test built the wrong tracks.** `build_project`
  returns video+audio; the "topmost wins" case put its second clip
  on track 1 — the audio track — and then expected video topmost
  semantics. The implementation was right; the fixture was wrong.
  Lesson: name fixtures after the arrangement they build.
- **Wrong offset in the source-math expectation.** Expected
  `source_in + position` where the mapping computes
  `source_in + (position − clip_position)`. Off-by-clip-start —
  the kind of error that looks like a decoder bug until you write
  the units down.
- **A missing comma hid inside a multi-line assert.** A miscounted
  line-range edit then landed on the wrong test. All repaired with
  exact-anchor edits; the format gate and the full suite pass on
  the final tree.

## 9. What is next

Undo/redo lands on the command seams next, then snapping and ripple
edits reuse the clamp math. The deferred transport commands
(shuttle/multi-speed, marks, looping) build directly on the
generation-token pattern — a rate knob is one more field on the
anchor. True sample-accurate A/V sync needs real-device measurement
that a dummy-driver CI cannot provide; the drift snap/slew machinery
is the seam it will plug into.

## 10. Five review questions (with answers)

1. **Why does `tick()` return the next deadline instead of the UI
   just timing itself?** The frame interval belongs to the sequence
   (rational, exactly 40 000 µs at 25 fps) and the schedule belongs
   to whoever owns the loop. Returning the deadline keeps the
   session GTK-free and loop-agnostic while making drift impossible
   at the source.
2. **How can a seek mid-decode not glitch?** It can — but it cannot
   *lie*: the generation bump means stale audio is dropped, the SDL
   queue is flushed before re-anchoring, and the video decoder is
   flushed so no frame from the old position survives. What plays
   after the seek is from the seek's position.
3. **Why does the empty sequence return TRUE from play?** "Nothing
   to play" is a legitimate state, not an error: the reporter seam
   tells the user, the state machine stays stopped, and callers
   keep one happy path. Errors are for things that could not be
   attempted.
4. **Why is `is_dummy` part of the adapter contract rather than
   hidden?** Because queue depth means different things: on a real
   device it is device time; on the dummy driver it is bookkeeping.
   The session uses the flag to decide whether drift correction
   against the queue is meaningful.
5. **What stops the monitor from holding a dangling frame after a
   project replace?** Ownership is explicit: frames are owned
   buffers, the monitor frees the previous one when a new arrives,
   and the session — the only producer — is destroyed before the
   project during replace, in the window's teardown order.
