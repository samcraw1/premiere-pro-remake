# Glossary

Vocabulary used in Obvious Edit code, docs, and reviews. Terms are listed
in the sense THIS project uses them.

## NLE (non-linear editor) terms

**Clip** — a reference to a range of source media placed on a timeline.
Obvious Edit clips are non-destructive: they never alter source files.

**Timeline** — the ordered arrangement of clips across tracks in
sequence time.

**Track** — a parallel lane of the timeline (video or audio). Clips on
one track play back in order; tracks play back simultaneously.

**Trim** — changing where a clip starts or ends relative to its source
media. Trims affect the timeline, never the source.

**Ripple** — an edit that shifts subsequent clips on the track to
preserve (or deliberately not preserve) sync.

**Snapping** — magnetic alignment of a clip edge to another clip edge,
playhead, or marker within a tolerance.

**Ripple edit** — see Trim and Ripple; the combination that closes gaps
automatically after removing material.

## Compositing terms (Phase 9)

**Compositing** — combining every covering video track's clip into one
frame. Track order IS the layering order: the compositor blends
ascending, bottom track first, so the highest-index track wins where
clips overlap.

**Visual properties** — the per-clip transform state (`OeClipVisual`):
position offsets, uniform scale, rotation, opacity, and crop. Owned by
the clip, deep-copied with it, and edited only through the validated
mutator.

**Identity visual** — the all-default visual: position (0, 0), scale
1000‰, rotation 0, opacity 255, no crop. A clip with the identity
visual renders exactly as a pre-Phase-9 clip did; "zero-value equals
today's behavior" is the model's compatibility contract.

**Permille scale** — uniform scale expressed in thousandths (1000 =
1.0×) so the model and the JSON stay integer-only: `scale-permille:
1250`, never a float like 1.25.

**Centidegree** — rotation in hundredths of a degree (`rotation-cdeg:
900` = 9.0°), keeping angles exact in integers.

**Src-over** — the straight, non-premultiplied alpha blend the
compositor uses: `out = (src·a + dst·(255−a)) / 255` per channel,
computed in integers and exact to within ±1 of the formula.

**Fast path** — the single-covering-clip, identity-visual render that
goes through the untouched Phase 8 pipeline (box-fit + centered copy)
byte-identically. The straight-cut parity test pins it.

**Stroke** — one pointer drag from press to release. A stroke previews
through unrecorded model mutations and commits exactly ONE undo
record carrying the visual captured at the stroke's first change (the
stroke baseline), never the last preview state.

**Clip gain** — the per-clip audio level (`OeClipAudio.gain`), fixed
point on the 1024 scale: 0 = silence, 1024 = unity, 2048 = +6 dB.
Owned by the clip beside its visual, deep-copied with it, and edited
only through the validated mutator.

**Clip pan / track pan** — the per-clip and per-track audio position
on the 1024 scale: 0 = hard left, 512 = center, 1024 = hard right.
The pan pair is linear (`2 × (1024 − pan)` and `2 × pan`), so the L+R
sum is constant at every position.

**Center unity** — the pan law's compatibility contract: a centered
pan contributes exactly 1024 per channel, so a centered channel sits
−6 dB below a hard-panned one and an all-default project mixes
exactly as it did before audio state existed.

**Track audio state** — `OeTrackAudio` on audio tracks only:
fixed-point volume (0–2048), pan (0–1024), and binary mute/solo.
Video tracks carry no audio state and the mutators reject them.

**Mute/solo matrix** — the track-level audibility rule (D5): with ANY
audio track soloed, only soloed tracks contribute; with none soloed,
mute zeroes the track. Serialized project state honored identically
by preview and export — never monitoring-only.

**Factor chain** — the one shared integer scaling every audio
contribution: `fade × clip gain × clip pan pair × track volume ×
track pan pair` per channel, all fixed point with unity 1024, one
64-bit product and one final rounding. The export mixdown and (from
Wave B) the playback mixer consume the same chain, so they cannot
drift.

**Mix window** — the span of sequence time (one decode ahead of the
playhead) for which the playback session sums every audible audio
track's contribution — in track-array order, gaps silent — into one
interleaved f32 buffer for the queue. The parity seam: the same
buffer feeds the speakers, the meter, and the parity test.

**Peak meter** — the per-channel level display beside the monitor.
Peaks are extracted per chunk from the mixed buffer on the main
context (no locks), held with a short decay, and released to silence
on pause, stop, and scrub — no paused tap, no timer while paused.

## Media terms

**Source media** — a file (video, audio, or image) referenced by the
project. Imported once, referenced by clips.

**Probe** — reading a media file's metadata (streams, codecs, durations)
without decoding frames. Phase 0's FFmpeg adapter exists so later probe
code has a live library under it.

**Decode** — turning compressed media into raw frames/samples.

**Probe** (Phase 2, as implemented) — `src/media/oe_probe.c` opens a file
with libavformat, classifies it (video / audio / still image), and fills
an `OeProbeInfo` with metadata: integer-microsecond duration, dimensions,
rational frame rate, sample rate, channels, container, and codec names.
Missing files give `OE_PROBE_ERROR_OPEN_FAILED`; files with no decodable
audio or video stream give `OE_PROBE_ERROR_UNSUPPORTED`.

**Thumbnail** — a small raw-RGBA preview decoded from an asset (seek to
10% of the duration capped at 3 s, first frame as fallback), scaled to
fit a 96×96 box preserving aspect. Stored as owned bytes in the asset
record and cached; the bin turns them into `GdkMemoryTexture`s.

**Waveform** — the compressed loudness profile of an asset's first audio
channel: a fixed number of min/max peak pairs, one pair per bucket,
produced through swresample + FFmpeg decoding and cached with the
thumbnail. Rendered by later phases; computed and stored now.

**Relink** — re-pointing an asset record at a new file path after its
original disappeared. The record returns to IMPORTING and is re-probed
like a fresh import; a relink that lands on another unsupported file
leaves the row in place as UNSUPPORTED so it can be tried again.

**Asset missing** — the status of an asset whose file existed at import
but was deleted or moved externally. The bin keeps the row (with a
Relink button) because the user may want to restore it; the record
holds the last known metadata.

**Resample** — converting audio between sample rates or channel layouts.

**Proxy media** — lighter-weight stand-in files used during editing,
swapped for the originals at export.

## GTK/GLib terms

**GType / GObject** — GLib's type system and object model. Obvious Edit
uses final types (`G_DECLARE_FINAL_TYPE`) for classes with no
subclassing plan.

**vfunc** — a virtual function slot on a GObject class (for example
`startup`, `activate`, `shutdown` on `GApplicationClass`). Overriding a
vfunc is how an application plugs behavior into the framework.

**GError** — GLib's error convention: fallible functions take a
`GError **` last parameter, return a boolean/object, and set `*error`
on failure. Phase 0's lifecycle adapters follow it strictly.

**Structured logging** — GLib's `g_log_structured()` family: records
carry key–value fields (PRIORITY, GLIB_DOMAIN, MESSAGE) instead of one
formatted string. Obvious Edit's single domain is `"oe"`.

**G_LOG_DOMAIN** — a compile-time string identifying the logging
domain. Set in meson.build so every file shares one domain.

**Map (signal)** — the GTK "map" signal fires when a widget becomes
visible on screen. The --self-check quits on the window's first map.

**Main loop** — GLib's event loop. GTK applications run one inside
`g_application_run()`.

## Shell terms (Phase 1)

**Media bin** — the panel listing the session's imported assets: one row
per asset with its thumbnail, name, kind+duration line, and status
badge. Rows for MISSING or UNSUPPORTED assets carry a Relink button;
dropping files onto the panel imports them.

**Source monitor** — the panel that will play the clip being examined
from the media bin, before it is edited into the timeline. In Phase 2
it still shows its labeled empty state — source playback is a later
phase.

**Program monitor** — the panel that will play the sequence itself —
what the timeline produces (Phase 3).

**Inspector** — the panel showing properties of the current selection.
In Phase 2 a bin selection fills it with the full probed record
(metadata, codecs, path); direct editing of clip properties (opacity,
volume, speed, …) arrives with the editing phases.

**Transport controls** — the play/stop/shuttle/mark buttons bound to
the transport commands; they live in the toolbar and the timeline area.

**Status bar** — the strip at the bottom of the shell where command
dispatch feedback appears ("'x' not implemented yet").

**Empty state** — the labeled placeholder a panel shows instead of a
blank canvas, naming what belongs there and when it arrives.

## Command terms (Phase 1)

**Command** — one named user action of the shell (play, undo, import).
Commands are registered in the GTK-free registry, not in widget code.

**Command ID (OeCommandId)** — the stable enum value identifying a
command across phases. Permanent API: dispatch and registration use
IDs; only presentation uses names.

**Dotted name** — the permanent string name of a command
(`transport.play-pause`), used in the action namespace (`app.<name>`),
logs, and reports. Renaming later is a breaking change.

**Accelerator** — the default keybinding for a command (Space, J/K/L,
I/O, V, C, Delete; Ctrl+Z / Ctrl+Shift+Z are reserved for Undo/Redo),
stored in the registry and mapped to `app.<name>` actions by the
application layer.

**Dispatch** — running a command through the registry: enablement
check, then handler or not-implemented path, then reporter + log.
Dispatch is total; no path crashes or hangs.

**Reporter** — the registry's output seam: a function pointer the
registry calls with user-facing dispatch feedback. The status bar
subscribes as the reporter; the registry never knows GTK exists.

**Enablement** — the per-command on/off state checked before the
handler runs; disabled commands report and log instead of executing.

**GTK-free test** — a unit test that links no GTK and needs no display.
Command-table integrity, dispatch paths, and layout persistence are
all covered this way so CI can run them headlessly.

## Document model terms (Phase 3)

**Sequence time** — the coordinate system of the composed timeline,
counted in integer microseconds from the sequence start, driven by the
sequence's rational frame rate. Sequence time is what a position means;
it is independent of any particular source file's timebase.

**Position** — where a clip sits in sequence time: the sequence-time
offset of the clip's first frame on its track. Positions are integer
microseconds and clips on one track are stored sorted by position.

**In/out point** — the pair `source-in-us` / `source-out-us`: the
half-open range `[in, out)` of the source media a clip references. The
timeline duration of every clip — stills included — is
`source-out-us − source-in-us` exactly.

**Media reference** — the file-stable handle a project file uses to
name source media: a unique positive integer (`ref`) paired with a
path, owned by the project. Clips point at references, never directly
at files, so a relink re-points every clip that uses the source. Media
references persist in project files; the Phase 2 session asset ids are
transient and never serialize.

**Format version** — the `format-version` integer, the first member
inside the document root. It exists so readers can refuse documents
they do not understand: anything other than the supported version is a
typed error, never a best-effort import.

**Migration** — converting a document from one format version to
another on load. v1 defines no migration; when a future version needs
one, it will be an explicit, reported transformation — silently
dropping or guessing members is forbidden by the strict-reader rule.

## Timeline terms (Phase 4)

**Playhead** — the widget-session cursor marking the timeline position
where playback or insertion happens. Session state only: it lives in
the timeline widget, never in the project model, and is never
serialized. Since Phase 5 it shows the playback session's position
(see [Clock](#playback-terms-phase-5) below) and dragging it feeds a
seek back into the session; it stays a view artifact, never model
state.

**Selection** — the widget-session designation of one clip as the
target of commands (Delete, zoom-independent highlight painting).
The timeline paints it; the project model has no field for it. The
Delete command commits through the model's remove mutator and then
clears the selection, so the model always stays authoritative.

**Zoom** — the widget-session pixels-per-microsecond scale relating
sequence time to screen space, with no model representation. Zooming
anchors on a fixed time point (the pointer for Ctrl+wheel, the widget
center for the zoom commands) so the time under the anchor stays put
on screen. Session-only: nothing in a project file records zoom.

## History terms (Phase 6)

**Command-object history (undo stack)** — every accepted edit becomes a
record holding everything needed to apply its inverse: insert/delete
carry a deep-copied [clip](#document-model-terms-phase-3), move/trim
carry old and new µs bounds. The GTK-free stack in `oe_undo_stack`
owns the records; the [project model](#document-model-terms-phase-3)
owns validity.

**Recorder helpers (`oe_edit_*`)** — the sole undoable path for model
edits: each helper performs the `oe_project_*` mutator call and pushes
a record only on success. A typed rejection (OVERLAP, BAD_RANGE,
bad index) records nothing, so the stack only ever holds history that
actually happened. Non-recording writers above the model are the
documented reason undo application must survive rejection.

**Strict LIFO** — undo and redo walk the records in exact reverse or
forward order of application, one step at a time, with no branching:
recording a new edit after an undo discards the redo branch
(linear history, no trees).

**Depth cap** — the history holds at most 100 records; recording past
the cap drops the oldest. Eviction removes history only — model
content created by an evicted record stays put.

**Apply-time rejection** — an inverse that the model refuses when the
stack applies it (for example after a direct, non-recording model
poke). The typed error propagates and the stack position stays
untouched, so the same step can be retried once the interference is
cleared.

**Auto-pause** — undo/redo while the session is
[playing](#playback-terms-phase-5) pauses the transport first, then
applies the inverse; the next play re-copies the mutated project, so
playback never holds a stale snapshot. History never rewinds the
playhead or crosses a project boundary (open/new clear the stack).

## Build terms

**Meson / Ninja** — the build configuration system and the low-level
build executor. Meson generates Ninja files from `meson.build`.

**werror** — treat compiler warnings as errors. Phase 0 builds must be
warning-free.

**Sanitizer** — compiler-instrumented runtime checking. Phase 0 gates on
`b_sanitize=address,undefined` (AddressSanitizer + UndefinedBehaviorSanitizer).

**Still reachable (Valgrind)** — memory still pointed at when the
process ends (typically one-time framework allocations). Not a leak
error; our Valgrind suppression file covers only GLib/GObject internals.

**Suppression file** — a Valgrind config listing known-safe allocation
patterns to exclude from error reporting. Ours is scoped by policy to
GLib/GObject internals only.

## Playback terms (Phase 5)

**Clock** — the playback session's notion of "where are we": a
wall-clock-anchored position in integer microseconds, recomputed from
the anchor on demand and re-anchored by play, pause-resume, and seek
so nothing ever accumulates drift. The clock is GTK-free state inside
`oe_playback_session` — widgets only read it (the
[playhead](#timeline-terms-phase-4) is its on-screen shadow) and feed
it seeks.

**A/V sync** — keeping what you hear and what you see pointed at the
same source microsecond. The session maps every tick position to one
clip per lane, decodes audio ahead through the worker, decodes video
frame-at-time, and nudges the visible position toward the audio queue
when a real device makes the two diverge. Under SDL's dummy driver
there is no device time, so verification of true sync is deferred to
real hardware (the PR's honest-limits note says so).

**Frame pacing** — when the next frame shows. The session's
`tick()` returns the next deadline in monotonic microseconds (one
frame interval at the sequence rate); the window schedules its GSource
at exactly that deadline instead of guessing an interval, so ticks
stay anchored even when a draw runs late.


## Editing terms (Phase 7)

**Snapping** — the editor's habit of quietly correcting a drag to
land on a meaningful time. A candidate microseconds from a clip
edge, the playhead, zero, or a frame boundary becomes that target
exactly; anything farther than the [threshold](#editing-terms-phase-7)
keeps its raw position. The decision lives in the GTK-free
`oe_timeline_snap_time` over an `OeSnapContext`, so the same rule
governs moves and trims and is unit-tested without a display.

**Snap threshold** — how many screen pixels of "close enough" a
drag forgives. It is a distance in pixels, not in time: through
`px_per_us` the same 8 px band widens as you zoom out and narrows
as you zoom in, so the magnetic feel is constant across zoom
levels. The band is inclusive at exactly the threshold.

**Frame grid** — the sequence's frame boundaries (from the
sequence rate) as snap targets, so a dragged clip can always land
frame-aligned even when no other clip is near.

**Tie-break** — when two targets are equally near a candidate, the
earlier time wins. Deterministic ties keep a drag from flickering
between two equally good answers.

**Ripple edit** — an edit whose effect flows sideways to keep the
rest of the track in step. Phase 7 ships one: **ripple delete**, where
removing a clip shifts every later clip on the same track left by
the removed clip's duration — rigidly, so the gaps between the
survivors survive. Clips on other tracks never move.

**Composite record** — one history record that stands for a
multi-step edit. The ripple delete records the primary clip's owned
copy plus each shifted neighbour's pre/post positions and indices
(removal renumbers downstream indices, so both generations are
recorded); undo and redo replay it through the ordinary typed
mutators. One action, one undo step, one depth unit in the stack.

## Export terms (Phase 8)

**Render seam** — the GTK-free frame-at-time path (`src/media/oe_render`)
shared by the program monitor and the export loop: map a position to
the topmost covering video clip, decode with one sequential decoder per
source path, composite into an owned opaque BGRA canvas. One path means
preview and export cannot disagree.

**Frame grid** — the export sampling rule: total frames is
`ceil(sequence_end_us / frame_interval_us)`, and frame *f* is sampled
at `oe_time_frame_to_us(f, rate)`. Durations come out within one frame
of the grid by construction, and probe checks can hold the export to
that bound.

**Mixdown** — the export audio render: every audio track decoded in
array order to 48 kHz stereo interleaved float, summed additively (no
per-clip gain exists in the model), hard-clamped to ±1.0. Gaps in a
track contribute silence.

**Atomic finalize** — the export twin of atomic project saves: the
muxer writes to a `g_mkstemp` temp in the destination's directory,
fsyncs it, and renames it over the destination only on full success.
Cancellation and every failure mode unlink the temp; a pre-existing
destination file stays byte-identical.

**Quality preset** — the named CRF mapping exposed by the chooser:
draft → CRF 28, good → CRF 23, quality → CRF 18, all at x264 preset
`veryfast`. CRF lower is better-looking and bigger; the preset hides
the knobs without hiding the trade.

## Project-specific terms

**Adapter** — a thin module wrapping one external library's global
lifecycle (init/shutdown) behind the GError pattern. `oe_ffmpeg` and
`oe_audio_output` are the two Phase 0 adapters.

**Self-check** — `obvious-edit --self-check`: open the window, quit
after the first map, exit 0. The repeatable proof of life.

**Keyframe** — a timed sample for one keyframeable visual property
(opacity, pos-x/pos-y, scale, rotation) on a clip, stored clip-relative
in raw microseconds. Between samples the value is linearly interpolated
with exactly one rounding at the final step; outside the keyed range it
clamps to the nearest endpoint; a degenerate store degrades to the
clip's static value.

**Transition** — a boundary object between two adjacent video clips:
`at_us` (the shared boundary), `duration_us` (the window centered on
it, clamped to both clips), and a kind. It exists only while both
neighbors still cover the window; any moved or trimmed clip degrades
it to the straight cut at composite time.

**Cross-dissolve** — the transition kind that blends the two clips
over the window with the shared integer ramp,
`out = (A*(255 − w) + B*w)/255` per channel.

**Dip-to-black** — the transition kind that runs the same ramp
through a pinned-black intermediate: A fades to black, black fades
to B.

**Fade envelope** — the shared linear integer audio ramp on a 0–1024
scale (`oe_fade_gain`), consumed by both the export mixdown and the
playback chunk path so preview and export cannot drift. Contribution
per sample: `(sample*g + 512) >> 10`, applied before the hard clamp.

**Generator clip** — a clip whose pixels are produced by the editor
rather than decoded from a file: a closed kind of `media`, `title`,
or `solid`. It renders as an ordinary layer (crop, scale, rotate,
opacity all apply), carries no audio, and keys over black inside a
transition window.

**Title** — a generator clip rasterizing owned UTF-8 text once per
(text, size, color, identity) at sequence resolution with the Cairo
toy API and the pinned 'DejaVu Sans' reference family, centered on
the frame; its size is the title height as permille of frame height.

**Solid** — a generator clip filling its layer with one packed
`0xRRGGBB` color; the simplest possible layer and the reference
case for generator compositing.

**Chroma key** — the per-clip keying of a video-track media clip by
color distance, computed in source space after crop and before
scaling as an ALPHA-ONLY rewrite: pixels within tolerance of the
key color go transparent, pixels beyond tolerance plus softness
stay opaque, and the band between rounds once through the house
ratio helper. RGB channels are never touched; no spill suppression.

**RGB distance** — the integer color metric behind the chroma key:
straight-line distance between two RGB triples on the 0–255
per-channel scale, computed exactly in integers so the tolerance
and softness domains (0–1024 on the doubled scale) are stable
across preview and export.

**Raster cache** — the render-session-owned store of generated
buffers, keyed by clip identity plus generator and render
dimensions, dropped on every sequence-snapshot refresh so an edited
title repaints fresh on the paused monitor.

**OE** — the project's module prefix (Obvious Edit) and its logging
domain.
