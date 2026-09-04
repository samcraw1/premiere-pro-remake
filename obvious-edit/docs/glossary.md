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

## Project-specific terms

**Adapter** — a thin module wrapping one external library's global
lifecycle (init/shutdown) behind the GError pattern. `oe_ffmpeg` and
`oe_audio_output` are the two Phase 0 adapters.

**Self-check** — `obvious-edit --self-check`: open the window, quit
after the first map, exit 0. The repeatable proof of life.

**OE** — the project's module prefix (Obvious Edit) and its logging
domain.
