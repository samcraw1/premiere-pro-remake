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

**Media bin** — the panel that will list imported media (Phase 2).
Until then it shows its labeled empty state.

**Source monitor** — the panel that will play the clip being examined
from the media bin, before it is edited into the timeline (Phase 2).

**Program monitor** — the panel that will play the sequence itself —
what the timeline produces (Phase 3).

**Inspector** — the panel that will show properties of the current
selection (opacity, volume, speed, …) for direct editing.

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
