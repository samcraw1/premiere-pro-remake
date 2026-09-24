# Obvious Edit

A native GTK4 non-linear video editor for Linux and macOS, written in C17.

This repository is the home of **Obvious Edit**. All code, docs, and build
tooling live in [`obvious-edit/`](obvious-edit/). The editor is built phase
by phase; every phase lands with headless tests, sanitizer and Valgrind
gates, and a guided walkthrough for new contributors.

## What works today

Eleven phases are complete. The editor currently:

- **Opens a themed editor shell** with a media bin, program monitor,
  inspector, timeline, transport, audio mixer, and status bar. Panel sizes
  persist to `~/.config/obvious-edit/layout.conf`.
- **Imports media** via `Ctrl+I` or drag-and-drop into the bin. A single
  decode thread probes files with FFmpeg and generates thumbnails and audio
  waveforms, cached under the user cache directory. Missing files are
  detected live and can be relinked.
- **Holds a project document** of tracks and clips with integer-microsecond
  positions, rational frame rates, and a sequence resolution. Projects save
  and load as strict JSON v1 files with the `.oe` extension, written
  atomically. Newer members backfill to identity values, so older files
  always load unchanged.
- **Edits on a timeline** with move and trim drags, snapping to clip edges,
  the playhead, and frame boundaries, plus ripple delete. The model stays
  authoritative: illegal edits are rejected with typed errors and the
  preview snaps back.
- **Composites layers** with per-clip crop, scale, rotation, position, and
  opacity, animated by keyframes, with transitions between clips and audio
  fades.
- **Mixes audio** across tracks through one integer factor chain: clip gain
  and pan, track volume and pan, mute and solo. Peak meters run live.
- **Generates content** with title and solid-color clips, and applies a
  chroma key to media clips on video tracks.
- **Plays back** with a wall-clock-anchored session clock corrected
  against the audio device, decode-ahead multi-track audio through SDL3
  queued contiguously from the playhead, and frame-at-time video
  rendered off the main thread into the monitor.
- **Exports** the timeline to an H.264/AAC MP4 through the same render seam
  the monitor uses, so preview and export cannot drift apart.
- **Undoes and redoes** every edit through a strict-LIFO command-object
  history capped at 100 entries.

## Keyboard commands

| Command | Shortcut |
|---|---|
| Play / pause | `Space` |
| Stop | `K` |
| Shuttle back / forward | `J` / `L` |
| Mark in / out | `I` / `O` |
| Select tool / razor tool | `V` / `C` |
| Toggle snapping | `S` |
| Delete selection | `Delete` |
| Undo / redo | `Ctrl+Z` / `Ctrl+Shift+Z` |
| Import media | `Ctrl+I` |
| Insert from bin | `Ctrl+E` |
| Zoom in / out | `Ctrl+=` / `Ctrl+-` |

On macOS the Command key replaces Control. New, open, save, export, and
title or solid insertion live in the menus. Menu, toolbar, and keyboard all
route through one command registry, so every entry point behaves the same.

## Architecture in one picture

```
src/main.c        entry point: logging init, app run
src/core/         document model, time, keyframes, fades, audio factors — GLib only
src/app/          application shell: commands, undo stack, media library, playback session
src/media/        FFmpeg adapters: probe, decode jobs, render seam, export, generators
src/playback/     SDL3 audio output adapter
src/ui/           GTK widgets: main window, bin, monitor, timeline, meters, theme
```

Three rules hold the layers apart:

1. **The UI never calls FFmpeg or SDL.** Everything the window learns about
   media arrives through the application layer.
2. **Adapters are symmetric.** Every init has an idempotent shutdown.
   Startup runs them in a fixed order and shutdown runs the exact reverse.
3. **The core layer knows no I/O libraries and no widgets**, so the whole
   document model is unit-tested without a display.

See [`docs/architecture.md`](obvious-edit/docs/architecture.md) for the
full picture, phase by phase.

## Building

### Debian 13 (trixie)

```bash
sudo apt-get install -y build-essential meson ninja-build clang-format valgrind xvfb dbus x11-utils libgtk-4-dev libglib2.0-dev libjson-glib-dev libavformat-dev libavcodec-dev libavutil-dev libavfilter-dev libswscale-dev libswresample-dev libsdl3-dev pkg-config
```

### macOS (Homebrew)

```bash
brew install gtk4 glib json-glib ffmpeg sdl3 cairo pkg-config meson ninja clang-format
```

### Compile

From `obvious-edit/` on either platform:

```bash
meson setup build && ninja -C build
```

The build uses `-Werror` at warning level 2 and must compile with zero
warnings. Meson enforces minimum versions for every dependency at configure
time. Exact pinned package versions and platform notes are in
[`obvious-edit/README.md`](obvious-edit/README.md).

## Running

```bash
./build/obvious-edit
```

The self-check starts the app, quits on the first window map, and exits 0.
On Linux it runs under Xvfb with a private session bus. On macOS it runs
on the logged-in display:

```bash
./scripts/run-headless.sh
```

Set `OE_LOG_LEVEL` to `error`, `warning`, `info`, or `debug` to change the
logging threshold.

## Testing

All 20 test suites are GTK-free and run without a display:

```bash
meson test -C build
```

They cover the lifecycle adapters, command registry, layout persistence,
media probe, decode jobs, media library, import worker, rational time,
project model and format, timeline geometry, snapping and ripple, playback
clock, audio output, audio tools, keyframes and transitions, export, titles
and chroma key, inspector strokes, and the undo stack.

### Verification gates

Every phase passes these before merging:

```bash
meson setup build-san -Db_sanitize=address,undefined && meson test -C build-san
```

```bash
meson test -C build --wrapper "valgrind --tool=memcheck --leak-check=full --error-exitcode=1 --suppressions=$(pwd)/tests/valgrind.supp"
```

```bash
clang-format --dry-run --Werror src/*.c src/*/*.c src/*/*.h tests/*.c
```

Valgrind does not support Apple Silicon, so the memcheck gate is Linux-only.

## Documentation

- [`docs/architecture.md`](obvious-edit/docs/architecture.md) — how the layers fit and the rules between them
- [`docs/code-map.md`](obvious-edit/docs/code-map.md) — what lives in which file
- [`docs/project-format.md`](obvious-edit/docs/project-format.md) — the `.oe` JSON v1 schema
- [`docs/glossary.md`](obvious-edit/docs/glossary.md) — NLE vocabulary used in the codebase
- [`docs/learning/`](obvious-edit/docs/learning/) — guided walkthroughs for phases 0 through 11, each with per-file explanations, a call flow, an ownership table, a bug log, and review questions

## License

MIT. See [`obvious-edit/LICENSE`](obvious-edit/LICENSE).
