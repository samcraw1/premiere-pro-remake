# Architecture (Phases 1–7)

## The shape of the system

Phase 0 built the load-bearing frame; Phase 1 hung the editor shell on
it; Phase 2 fed it media; Phase 3 gave it a document — the project &
timeline data model in `src/core/`, GTK-free and headlessly tested. The
current shape:

```
obvious-edit (binary)
 └── OeApplication (app/oe_application.[ch])
      ├── startup vfunc, in order:   shutdown vfunc, reverse order:
      │    oe_ffmpeg_init() [g_once]    oe_audio_output_shutdown()
      │    oe_audio_output_init()       oe_ffmpeg_shutdown()
      │    oe_theme_init()              (worker joins in window dispose,
      │    install command actions       BEFORE oe_ffmpeg_shutdown)
      │
      └── activate vfunc
           └── OeMainWindow (ui/oe_main_window.[ch])
                titled "Obvious Edit", size from layout.conf
                menu bar + toolbar → app.<command> actions
                bin_paned (H): media bin | timeline_paned (V)
                    ├─ inspector_paned (H): monitors | inspector
                    └─ timeline area + transport
                status bar ← command reporter
                │
                │ owns (GTK-free, app/):
                ├── OeMediaLibrary — session assets, statuses, observers,
                │       GFileMonitor per OK asset, relink
                └── OeImportWorker — one GThread + GAsyncQueue,
                        results via g_main_context_invoke
                        ↕ (plain buffers, no GTK types)
                     media/ (FFmpeg only): oe_probe, oe_media_jobs
                        ↕ raw-binary, keyed path+size+mtime
                     app/oe_media_cache ($XDG_CACHE_HOME)
```

## Layers and their rules

| Layer | Directory | Talks to | May not |
|---|---|---|---|
| Entry point | `src/main.c` | OeApplication, oe_log | touch GTK widgets |
| Core model | `src/core/` | GLib/GObject/json-glib only | touch GTK, FFmpeg, or SDL |
| Application shell | `src/app/` | all adapters, UI, core model | decode media |
| Media adapters | `src/media/` | FFmpeg only | touch GTK or SDL |
| Playback adapters | `src/playback/` | SDL3 only | touch GTK or FFmpeg |
| UI | `src/ui/` | GTK only | touch FFmpeg/SDL directly |

Three rules follow from this table and are enforced by review, not
tooling:

1. **UI never calls FFmpeg or SDL.** Everything the window learns about
   media — and the project document itself — arrives through the
   application layer.
2. **Adapters are symmetric.** Every `*_init` has a paired `*_shutdown`,
   both idempotent, both safe to call in any state. Startup runs them in
   a fixed order; shutdown runs the exact reverse.
3. **The core layer knows no I/O libraries and no widgets.**
   `src/core/` is plain GLib/GObject plus json-glib for the format
   module. It never includes gtk.h, FFmpeg, or SDL headers, so the
   whole document model is unit-tested headlessly and could outlive the
   front-end unchanged.

## Why the lifecycle adapters exist

FFmpeg's global state and SDL's subsystems both need one-time setup and
symmetric teardown. Wrapping each in a GError-pattern adapter gives us:

- **A single owner of init state.** The application vfuncs are the only
  production callers; tests can call the adapters directly.
- **Failure isolation.** If audio init fails, FFmpeg is already up, and
  shutdown still unwinds both in reverse.
- **A seam for testing.** The smoke tests drive init/shutdown twice and
  in pathological orders without a display.

## Structured logging

`app/oe_log.[ch]` is the whole project's single logging domain
(`G_LOG_DOMAIN="oe"`, set in meson.build). Every log record goes through
`g_log_structured_array()` with explicit PRIORITY / GLIB_DOMAIN / MESSAGE
fields. `OE_LOG_LEVEL` (error|warning|info|debug) overrides the emission
threshold and is re-read on `oe_log_init()`, so tests can retune it.

## The self-check contract

`obvious-edit --self-check` is the Phase 0 acceptance behavior:

1. GTK starts normally (real startup path, real adapters).
2. `activate` creates the main window and connects a one-shot `map`
   handler before presenting it.
3. On the window's first map, the handler calls `g_application_quit()`.
4. `shutdown` runs the adapters in reverse; `main` exits 0.

The same startup/shutdown code runs for interactive use — the self-check
is not a special mode with its own lifecycle.

## The shell layer (Phase 1)

Three new seams extend the frame. All follow the same rule: GTK-free
logic, unit-tested headlessly, with widgets as thin adapters.

**Command routing (`src/app/oe_command.[ch]`).** A GTK-free registry of
19 commands — stable `OeCommandId` enum plus permanent dotted names
(`transport.play-pause`, `edit.undo`, …) and default accelerators
(Space, J/K/L, I/O, V, C, Delete; Ctrl+Z / Ctrl+Shift+Z reserved for
Undo/Redo). The application layer installs one `GSimpleAction` per
command (`app.<name>`) and maps accelerators; menu, toolbar, and
keyboard all route through the same action path. Dispatch is total:
unknown, disabled, and not-yet-implemented commands all report via the
reporter seam, log through oe_log, and never crash or hang. The status
bar subscribes as the reporter; the registry never includes gtk.h.

**Original theme (`src/ui/oe_theme.[ch]` + CSS resource).** One
stylesheet compiled into the binary via GResource — the shell cannot
come up unstyled. Applied idempotently through `GtkCssProvider` on the
default display. No libadwaita; the palette is original and light-on-
dark with visible focus outlines.

**Layout persistence (`src/ui/oe_shell_layout.[ch]`).** A plain struct
(window size, maximized flag, three splitter positions) saved to
`$XDG_CONFIG_HOME/obvious-edit/layout.conf` as a versioned GKeyFile
(version group starts at 1), written atomically (temp file + rename).
Load failures degrade safely: missing file → documented defaults;
corrupt parse → defaults + warning; newer version → defaults + warning;
out-of-range fields → clamped. The widget layer only reads/writes the
struct — load at construction, apply splitter positions on first map,
save on close-request.

The shell composition itself uses only GtkPaned and GtkBox with shrink
disabled — no GtkFixed, no absolute pixel geometry — so panels resize
proportionally and empty states name the phase that will fill them.

## The media import pipeline (Phase 2)

Phase 2 adds the first layers that learn what a media file is. The
rule that shapes all of it: **the app layer orchestrates decode without
decoding** — it decides *whether* and *in what order*, while only
`src/media/` ever touches an FFmpeg API, and only `src/ui/` ever builds
a widget.

**Import paths converge.** The Ctrl+I chooser (`GtkFileDialog`
`open_multiple`, extension filters that only narrow the picker — the
probe is the accept/reject authority) and the bin's file-list drop
(`GtkDropTarget` + `GDK_TYPE_FILE_LIST`, copy action) both call one
`import_paths()` in the window. There is exactly one import entry
point; the gestures are adapters.

**Threading model.** One decode thread, created with the window. Jobs
are immutable and refcounted, queued through a `GAsyncQueue`. The
worker checks the cache first, runs probe → thumbnail → waveform with
an atomic cancellation check between steps, and delivers each result
with `g_main_context_invoke`, so the window's callback always runs on
the main thread. Shutdown frees the worker (drain + join) before the
application's `oe_ffmpeg_shutdown` — a thread must outlive nothing it
calls into.

**State lives in the library.** `oe_media_library` is the single owner
of asset records: opaque ids, IMPORTING / OK / MISSING / UNSUPPORTED,
deep-copied probe metadata, owned raw-RGBA thumbnails, and a GTK-free
observer. A `GFileMonitor` per OK asset flips records to MISSING when
files disappear; `relink()` re-points a record and returns it to
IMPORTING. The bin is a projection — it rebuilds its rows from the
library on every change and keeps no state of its own.

**Derived media is cached.** Thumbnail and waveform bytes are cached
under `$XDG_CACHE_HOME/obvious-edit/media/` keyed by canonical path +
size + mtime (no content hashing — the cache exists to avoid reading
every byte). Entries are magic-framed raw binary; corrupt entries are
misses; there is no eviction policy and deleting the directory is a
cold cache.

**Time and metadata floors.** Durations are integer microseconds,
frame rates are num/den rationals, no floats in metadata — the
project-format time-model floor is enforced from the first probe.

## The timeline widget (Phase 4)

The timeline is the first GTK surface that both reads and writes the
project model, so its layer rules are strict:

**The widget observes; it never holds model references.**
`oe_timeline` registers itself as the project's observer (the first
production consumer of the seam) and every repaint works on a fresh
`oe_project_get_sequence()` deep copy. Between notifications the widget
touches only its own snapshot, so a model mutation can never be
observed half-applied.

**Geometry is GTK-free.** `src/ui/oe_timeline_layout.[ch]` owns the
zoom state (`px_per_us`), microsecond↔pixel round-trips, lane
mapping, edge-band hit-testing, the pure move/trim clamp math, and
— since Phase 7 — the pure snap decision. It links GLib only, so
all of it is unit-tested headlessly in `test_timeline_layout` and
`test_snap_ripple` — the widget just feeds it pixels.

**One drag state machine, model mutators commit.** The gesture
controllers share a single state: press arms `move`, `trim-in`,
`trim-out`, `playhead`, or a click; motion previews the *clamped*
candidate without writing the model; release commits through
`oe_project_move_clip` / `oe_project_trim_clip`. A typed rejection is
reported through the window's status seam and the preview snaps back —
the model stays authoritative and always legal.

**Session state stays out of the model.** The playhead, zoom, and
selection are widget-session state. The playhead has no model field
and is never serialized (the Phase 5 playback clock owns time); zoom
is view state only. Missing media is resolved through a window-supplied
callback backed by the session library — hatched rendering, no
probing during draws — and missing media refuses trims.

## The playback clock (Phase 5)

Playback introduces the strictest ownership rules yet and a second
look at the editor's first clock discipline — integer microseconds:

**The session owns time; GTK owns nothing.** `src/app/
oe_playback_session.[ch]` is a GTK-free state machine over a project
snapshot: stopped / paused / playing, a wall-clock-anchored position
(integer µs), `tick()` returning the next frame deadline in monotonic
µs, and pause/resume/seek re-anchoring — drift accounting resets at
every transition. Every position a caller sees (observer callback,
`get_position`) is computed from the anchor, never accumulated, so
slow UI ticks cannot make the transport drift. End-of-sequence is
computed from a fresh `oe_project_get_sequence()` deep copy: a trim
that lands mid-playback changes when playback stops.

**Mapping is pure.** `oe_playback_session_map()` answers "what plays
here": topmost video track wins, spans are half-open, source µs is
clamped into the clip's source range. The session uses it per tick;
headless tests use it directly — the same function, no GTK.

**Decode-ahead audio, frame-at-time video.** `src/media/
oe_media_playback.[ch]` adds full-resolution decode: a worker thread
(GThread + GAsyncQueue) decodes interleaved f32 chunks ahead of the
transport and delivers them with `g_main_context_invoke` — the same
main-context discipline as the import worker, drained and joined
before `oe_ffmpeg_shutdown` — while video decodes frame-at-time to
packed BGRA at monitor resolution, sized so Cairo's ARGB32 surface
blits it without a per-frame channel swap. Requests carry a
generation token; a seek supersedes the in-flight request instead of
queueing behind it.

**SDL3 push audio behind the adapter.** `oe_audio_output` grows the
push-model device stream: open once on the default playback device,
queue interleaved f32, report queue depth, flush (the seek discipline:
flush the device, then re-anchor what plays next), pause/resume the
device, close before shutdown. The adapter stays the only SDL surface
the session sees, and the no-device path is a typed error — playback
continues wall-clock only, with `is_dummy` telling the session
whether queue depth is real.

**The UI is a viewport, not a clock.** The window installs one
GSource scheduled at the session's next deadline; `tick()` advances
the clock and pushes the playhead into the timeline, while the
program monitor (`oe_program_monitor`) blits the newest owned frame.
The widget never owns or computes time; dragging the playhead feeds a
seek back into the session through the timeline's playhead-changed
callback. Missing media is reported once per run (event + status
bar) and hatched in the monitor while the transport continues; an
empty sequence reports "nothing to play"; reaching the end stops
with the playhead parked on the final position.

## The command-object history (Phase 6)

Undo/redo is a command-object history over the project model — every
edit becomes a record whose inverse applies only through the same
typed mutators that created it:

**The stack owns history; the model owns validity.** `src/app/
oe_undo_stack.[ch]` is GTK-free and holds `OeUndoRecord` command
objects: insert/delete carry a deep-copied `OeClip`, move/trim carry
old and new µs bounds. The stack is strict LIFO, capped at depth 100
(oldest dropped), and any newly recorded edit discards the redo
branch — linear history, no trees. Undo/redo apply inverses ONLY
through `oe_project_*` mutators with typed `GError`s; a rejected
inverse propagates its error and leaves the stack position
untouched, so a failed undo can be retried after the interfering
edit is cleared. The module header states the sole-path invariant:
all edits that should be undoable go through the recorder helpers
(`oe_edit_insert_clip`, `oe_edit_remove_clip`, `oe_edit_move_clip`,
`oe_edit_trim_clip`), which perform the mutator call and push a
record only on success — typed rejections record nothing. The
documented escape hatch for future hard-to-invert mutators is a
full-model snapshot record.

**The UI is a thin hand on the seam.** `edit.undo` / `edit.redo`
actions call the stack; the `OeUndoChangedFunc` seam fires on every
history transition and drives command enablement via
`oe_command_set_enabled` — the buttons never poll. Each applied
step reports "Undo: <label>" / "Redo: <label>" (or "Nothing to
undo/redo") through the existing status-reporter seam, selection
clears after the jump, and history never rewinds the playhead.
When the session is PLAYING, undo/redo pauses first, then applies —
the next play re-copies the mutated project, so playback never
holds a stale snapshot. `reset_session` clears the stack on open
and new: history never crosses a project boundary.

## Snapping and ripple (Phase 7)

Two editing affordances land on top of the Phase 4 widget and the
Phase 6 history, both GTK-free at their core:

**The snap decision is pure math.** `oe_timeline_layout` owns
`OeSnapContext` — an enabled flag, a threshold in pixels, the zoom
(`px_per_us`), optional playhead and frame-grid targets, and the
same-track neighbour edges — plus `oe_timeline_snap_time`, which
returns the snapped candidate or the candidate unchanged. Targets
are same-track clip edges, the playhead, zero, and frame
boundaries; the nearest target wins and ties go to the earlier
time. The threshold (default 8 px) lives in screen space and
scales through `px_per_us`, so snapping feels constant at any
zoom. A disabled context is a pass-through, and the widget applies
snapping for move, trim-in, and trim-out BEFORE the legality
clamp — the clamp stays authoritative: snap proposes, clamp
disposes, and `commit_drag` keeps trusting the preview verbatim.
The decision is unit-tested in `test_snap_ripple` without a
display.

**The toggle is a wired command, not a setting.** `edit.snap-toggle`
(accel `s`) flips the widget's session flag through the
`oe_timeline_set/get_snapping` seam; the Edit menu renders it as a
stateful check action so the checkbox always reflects the widget's
truth, and the status bar reports "Snapping on/off" through the
existing status seam.

**Ripple delete is one action, one undo step.**
`oe_edit_ripple_remove_clip` removes the primary clip and shifts
every later clip on the same track left by the primary's duration —
rigidly, gaps preserved — all through the existing typed mutators.
It pushes ONE composite record (`OE_UNDO_OP_RIPPLE_DELETE`) that
carries the primary's owned copy plus each suffix clip's pre/post
positions and indices (the removal renumbers downstream indices,
so both generations are recorded). Undo and redo replay through
the same mutators — descending for undo, ascending for redo — so
every intermediate state stays typed-valid, the stack keeps its
strict-LIFO depth accounting (one composite record = one depth
unit), and a fresh edit after undo still discards the redo branch.
Record-time ripple while PLAYING leaves the session alone (move/
trim semantics); history application pauses first, exactly like
undo/redo.

## Export and the render seam (Phase 8)

Export closes the gap between what the monitor shows and what lands in
a file. The rule that shapes it: **preview and export share one render
path** — the same GTK-free seam that repaints the program monitor is
what samples the exported file, frame by frame, so a straight cut that
looks right on screen cannot silently render differently in the MP4.

The seam lives in `src/media/oe_render.[ch]`. A render source is a
borrowed deep-copied sequence snapshot plus one GTK-free callback — a
media-ref → owned-path resolver with its `gpointer` user-data
(`OeRenderSource`); the render session caches one sequential decoder
per source path (locked decision D2), so the export loop never seeks
backwards per frame — the cost that makes a naive per-frame seam
unusable. `oe_render_frame_at`
collects every covering video clip (a clip covers `t_us` when
`t_us ∈ [position, position + source_out − source_in)`, source time
`source_in + (t_us − clip_position)`, half-open clamped to
`[0, duration)`) and composites them into a freshly owned, opaque
BGRA canvas — one clip through the byte-identical fast path described
below, several through the layered compositor of the next section,
with aspect-preserving box-fit and even-dimension letterboxing per
layer.

The export job lives in `src/media/oe_export.[ch]` — synchronous,
GTK-free, the same calling convention as the media jobs (a
`GAsyncReadyCallback`-free run function with a hard cancel flag and a
`(done, total)` progress callback). Video: total frames is
`ceil(sequence_end_us / frame_interval_us)`, frame *f* sampled at
`oe_time_frame_to_us(f, rate)`, encoded H.264 via libx264 (by-name,
with `avcodec_find_encoder(AV_CODEC_ID_H264)` fallback), yuv420p,
CRF 18/23/28 from the draft/good/quality presets at x264 preset
`veryfast`. Audio: one additive mixdown across ALL audio tracks in
array order, decoded to interleaved float at 48 kHz stereo, summed,
hard-clamped to ±1.0 — gaps contribute silence.

Finalization copies the project-save pattern: a `g_mkstemp` temp in
the TARGET directory, custom AVIO over its fd, header → frame pump
(with a cancel check per frame) → trailer → fsync → `g_rename` over
the destination only on full success. Every other exit — encoder
failure, mux failure, cancellation — unlinks the temp and reports a
typed `OeExportError`; the destination file is created or left
byte-identical, never truncated.

The thread boundary is the import worker's, inverted: the window
serializes the state the job needs (a deep-copied sequence snapshot
plus the media-ref → path map), owns a dedicated worker thread, and
marshals progress/completion back through `g_main_context_invoke`.
A session-epoch tag discards stale completions after a project
switch; the atomic cancel flag is consulted between frames, so a
cancel press ends the job within one frame of encode.

## Compositing (Phase 9 Wave A)

Every clip owns an `OeClipVisual`, and identity is the zero value
except scale: `pos_x`/`pos_y` are frame-pixel offsets from the
centered anchor, `scale_permille` is uniform scale (1000 = 1.0×),
`rotation_cdeg` is rotation in hundredths of a degree, `opacity` runs
0–255, and `crop_l/t/r/b` are source-pixel insets. A freshly inserted
clip's visual is the identity, so a zero-value clip renders exactly
as every pre-Phase-9 clip did — there is no dormant state to forget.
`fade_in_us`/`fade_out_us` and the keyframe store exist in the struct
but are consumed in Wave B.

Layering is track order, full stop: the compositor blends covering
clips ascending — bottom track first — each one decoded, cropped
(source pixels, before scaling), scaled (nearest-neighbor), rotated
(integer bilinear resample with 8-bit weights, deterministic),
translated to its centered anchor plus the position offset, and
blended with straight non-premultiplied integer src-over
(`oe_render_blend_channel`, within ±1 of the alpha formula). An
opacity-0 layer skips out of the blend entirely. When exactly one
clip covers the position and its visual is the identity, the
untouched single-layer pipeline renders the frame byte-identically
to Phase 8 — the fast path the straight-cut parity test pins.

The monitor and export share this one seam. The playback session
owns a render session over its deep-copied sequence snapshot and
caches decoders exactly like the exporter, so preview and export
cannot disagree about what a timeline looks like. Visual edits go
through the validated mutator `oe_project_set_clip_visual` (typed
`OE_PROJECT_ERROR_BAD_VISUAL` rejections, one observer notification)
and commit as exactly ONE `OE_UNDO_OP_VISUAL` record per stroke via
`oe_edit_set_clip_visual` — undo restores the visual captured at the
stroke's first change, not the last preview state. A paused monitor
repaints the edited frame when the project notification fires. Wave
B fills in the keyframes, transitions, and fade envelopes below; the
model fields and the seam were already in place for them.

## Keyframes, transitions, and fades (Phase 9 Wave B)

Keyframes are per-property sorted `GArray`s of `{gint64 time_us;
gint32 value;}` inside the clip's visual, stored in raw microseconds
clip-relative and interpolated linearly through
`oe_keyframes_sample`: `value = va + oe_time_round_ratio ((gint64)
(vb - va) * (t - ta), tb - ta)` — one division, one rounding, at the
final step only. Times outside the run clamp to the first or last
entry's value; an empty, single-entry, unsorted, or zero-span store
degrades to the clip's static value. The keyframeable set v1 is
opacity plus the transform properties (pos-x/pos-y, scale, rotation);
crop stays static. The compositor resolves the per-frame value with
`oe_clip_visual_resolve` before the transform/opacity application, so
preview and export animate identically. Keyframe edits go through the
validated `oe_project_set_clip_keyframe`/`remove` mutators — the same
one-`OE_UNDO_OP_VISUAL`-per-stroke discipline as any other visual
edit, frame-snapped in the inspector only (no timeline gizmos).

Transitions are model objects — `{track_index (video tracks only),
at_us, duration_us, kind}` with `at_us` the shared boundary where
clip1's end equals clip2's start and the window centered on it,
clamped to both clips. The validated add/move/remove mutators reject
unknown tracks, boundaries without neighbor coverage, and non-positive
durations; the compositor runs the two-input blend over the window
with the same integer ramp as everything else: `out = (A*(255 - w) +
B*w)/255` per channel, dip-to-black running the same ramp against a
pinned-black intermediate. w = 0/255 and a zero-duration window
degrade to the straight cut, and a transition exists only while both
neighbors still cover the window — any moved or trimmed clip degrades
it gracefully at composite time, with no mutator fixups. The timeline
draws a shaded band at the boundary (GTK-free layout logic; the widget
just draws it), the band's edges join the snap-target list, and
ripple delete re-anchors `at_us` through the validated mutator as one
extra replay sub-step.

Fades share one GTK-free, FFmpeg-free envelope in `src/core/oe_fades.c`:
a linear integer ramp on a 0–1024 scale,
`g = MIN (1024, oe_time_round_ratio ((t - clip_start)*1024,
fade_in_us), oe_time_round_ratio ((clip_end - t)*1024,
fade_out_us))`, consumed by BOTH the export mixdown sum loop and the
playback chunk path — one implementation so preview and export cannot
drift — applied before the unchanged hard clamp.

Persistence follows the width/height backfill recipe: the clip-level
`keyframes` and track-level `transitions` members are emitted
unconditionally, absent-on-read backfills NONE (their absence means
none), the closed member lists extended at each level, integer tokens
only, no version bump, and save-load-save is byte-identical.

## Audio state and the factor chain (Phase 10 Wave A)

Every clip owns an `OeClipAudio` — fixed-point `gain` on the 1024
scale (0 = silence, 1024 = unity, 2048 = +6 dB) and `pan` on the 1024
scale (0 = hard left, 512 = center, 1024 = hard right) — and every
AUDIO track owns an `OeTrackAudio` with `volume` (0–2048), `pan`
(0–1024), `mute`, and `solo`. Video tracks carry no audio state at
all: the track-level mutators reject them with
`OE_PROJECT_ERROR_BAD_TRACK`. Both substructs are memory-free, so the
deep-copied-never-aliased ownership rule costs the copy trios one
extra value copy per struct. Like the visual, the identity is the
zero-value default: an all-default Phase 10 project mixes exactly
like a Phase 9 one.

One shared integer chain in `src/core/oe_audio_factor.c` scales every
audio contribution — the export mixdown today, the playback mixer
from Wave B:

    factor[ch] = fade x clip_gain x pan_pair(clip_pan)[ch]
                 x track_volume x pan_pair(track_pan)[ch]

All stages are fixed point with unity 1024 (the fade envelope's scale
convention); the product is computed exactly in 64-bit integers and
normalized with one rounding. The pan pair is linear —
`2 x (1024 - pan)` and `2 x pan` — so the L+R sum is constant and
CENTER PAN IS UNITY: a centered channel sits −6 dB below a
hard-panned one, and the identity pan contributes exactly 1024, which
is what keeps the all-default mix identical to Phase 9. Mute and
lose-solo zero the whole chain: `oe_audio_audible` resolves the
track-level matrix (any soloed audio track → only soloed ones
contribute; none soloed → mute zeroes the track) and the caller
passes the verdict in. The mixdown recomputes the chain per AVFrame
with the existing fade value — the envelope's cadence is unchanged —
and the single final hard clamp stays the last word. Integer end to
end: no floats in the model, the serialized state, or the chain.

The mixdown honors the matrix before touching media: a silenced
track is skipped before its source is even opened, so a muted project
exports exactly what it will play. Audio edits go through the
validated mutators `oe_project_set_clip_audio` /
`oe_project_set_track_audio` (typed `OE_PROJECT_ERROR_BAD_AUDIO`
rejections; validate first, deep-copy second, swap last, notify
last) and commit as one `OE_UNDO_OP_CLIP_AUDIO` /
`OE_UNDO_OP_TRACK_AUDIO` record per stroke — the track payload is
keyed by track index alone, no sentinel clip index, and a zero-delta
stroke records nothing. Persistence follows the backfill recipe: the
clip-level `audio` member on every clip and the track-level `audio`
member on every audio track, absence on read backfills the identity,
integer tokens only, closed member lists, no version bump, and
save-load-save is byte-identical.

## Multi-track playback, metering, mixer (Phase 10 Wave B)

Playback stops being a one-lane pick and becomes a true mixer. Each
feed cycle the playback session opens a MIX WINDOW — a span of
sequence time one decode ahead of the playhead — collects every
audible AUDIO track that intersects it (track-array order, higher
index later: the same order the export mixdown and the compositor
use), decodes each track's clip range sequentially through the one
media worker, and sums the scaled contributions into ONE interleaved
f32 buffer the session owns. Gaps in any track stay silent (zero);
track order is deterministic so two runs of the same project mix
bit-identically. Contributions scale through the SAME shared factor
chain (buffer-constant clip gain/pan x track volume/pan per channel,
mute/lose-solo verdicts passed in) while the fade envelope keeps its
per-sample cadence; the final clamp is the last word, exactly as in
the mixdown. Stale deliveries from superseded decodes are dropped by
generation, and the queue's progress gate tracks pushed coverage in
sequence time (the first window begins LOOKAHEAD ahead of the
playhead, so pushed frames do not measure coverage from the restart
position). The session layer stays GTK-free: mixer consumers bind
two observers — `set_mix_func` (the parity seam: called with the
finished window) and `set_meter_func` (per-chunk channel peaks).

Decoded chunks are labeled with their SOURCE time. The worker's
chunk buffer persists across decoder frames, so labels anchor to the
buffer (stamped when it is empty, advanced by the chunk length at
each delivery) rather than to per-frame arithmetic — a mid-frame
residual mislabeled late produces periodic gaps and overlaps in the
mapped stream, audible as level wobble under real mixing.

Metering (D6) is per-chunk peak extraction over the mixed buffer on
the GLib main context — no locks, no extra thread, GTK-free math
(`oe_audio_buffer.c` for per-channel peaks over interleaved f32,
`oe_meter_math.c` for the decay and bar geometry). The meter widget
holds each peak with a short documented decay and, because paused,
stopped, and scrubbed playback delivers no chunks, settles at zero
without a paused tap or a timer.

The shell gains an inspector stack page 'mixer' — one row per AUDIO
track: volume slider (0–2048, unity 1024), pan slider (0–1024,
center 512), mute and solo toggles — plus a clip-audio section on
the clip page (gain and pan). Both edit through the validated
mutators with the stroke pattern: preview without record, baseline
captured at stroke begin, exactly ONE undo record at commit,
zero-delta strokes record nothing. Repaints are notify-driven; no
refresh timers exist.

Preview and export parity is a standing test: a two-audio-track
project with distinct levels and pans plays through the virtual
clock (its mixed chunks captured via the mix observer) and must
match the export mixdown per channel. The test fails by construction
on single-lane playback; its pass is the proof the mixer landed.

## What comes later

`src/core/` is the foundation later phases build on, and the
adapter seams in `src/media/` and `src/playback/` are where media
and audio plug in. See `docs/learning/phase-0.md` through
`phase-9.md` for guided walkthroughs of each phase.
