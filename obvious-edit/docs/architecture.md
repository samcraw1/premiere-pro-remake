# Architecture (Phases 1–3)

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
16 commands — stable `OeCommandId` enum plus permanent dotted names
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
mapping, edge-band hit-testing, and the pure move/trim clamp math.
It links GLib only, so all of it is unit-tested headlessly in
`test_timeline_layout` — the widget just feeds it pixels.

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

## What comes later

The playback clock with audio output wiring (Phase 5), undo/redo
(after the playback clock), snapping, ripple edits, and export arrive
in later phases; `src/core/` is the foundation they all build on, and
the adapter seams in `src/media/` and `src/playback/` are where media
and audio plug in. See `docs/learning/phase-0.md` through `phase-4.md`
for guided walkthroughs of each phase.
