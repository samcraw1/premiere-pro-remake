# Architecture (Phase 1)

## The shape of the system

Phase 0 built the load-bearing frame; Phase 1 hung the editor shell on
it. The current shape:

```
obvious-edit (binary)
 └── OeApplication (app/oe_application.[ch])
      ├── startup vfunc, in order:   shutdown vfunc, reverse order:
      │    oe_ffmpeg_init()             oe_audio_output_shutdown()
      │    oe_audio_output_init()       oe_ffmpeg_shutdown()
      │    oe_theme_init()
      │    install command actions
      │
      └── activate vfunc
           └── OeMainWindow (ui/oe_main_window.[ch])
                titled "Obvious Edit", size from layout.conf
                menu bar + toolbar → app.<command> actions
                bin_paned (H): media bin | timeline_paned (V)
                    ├─ inspector_paned (H): monitors | inspector
                    └─ timeline area + transport
                status bar ← command reporter
```

## Layers and their rules

| Layer | Directory | Talks to | May not |
|---|---|---|---|
| Entry point | `src/main.c` | OeApplication, oe_log | touch GTK widgets |
| Application shell | `src/app/` | all adapters, UI | decode media |
| Media adapters | `src/media/` | FFmpeg only | touch GTK or SDL |
| Playback adapters | `src/playback/` | SDL3 only | touch GTK or FFmpeg |
| UI | `src/ui/` | GTK only | touch FFmpeg/SDL directly |

Two rules follow from this table and are enforced by review, not tooling:

1. **UI never calls FFmpeg or SDL.** Everything the window learns about
   media arrives through the application layer.
2. **Adapters are symmetric.** Every `*_init` has a paired `*_shutdown`,
   both idempotent, both safe to call in any state. Startup runs them in
   a fixed order; shutdown runs the exact reverse.

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

## What comes later

Editing engine, project model, playback clock, and persistence layers
arrive in later phases; the adapter seams in `src/media/` and
`src/playback/` are where they will plug in. See
`docs/learning/phase-0.md` and `phase-1.md` for guided walkthroughs of
each phase.
