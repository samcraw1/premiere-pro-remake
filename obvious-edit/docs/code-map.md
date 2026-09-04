# Code map (Phases 1–6)

What lives where, and why it lives there.

```
premiere-pro-remake/
├── README.md                      # pointer into obvious-edit/
└── obvious-edit/
    ├── meson.build                # targets, dependency floors, G_LOG_DOMAIN
    ├── meson_options.txt          # intentionally empty in Phase 0
    ├── .clang-format              # committed C style; CI enforces it
    ├── LICENSE                    # MIT
    ├── README.md                  # install line, pinned versions, gates
    ├── scripts/
    │   └── run-headless.sh        # dbus-run-session + xvfb-run self-check
    ├── src/
    │   ├── main.c                 # entry point: logging init, app run
    │   ├── core/
    │   │   ├── oe_time.[ch]       # rational time: reduced rates, µs conversions
    │   │   ├── oe_project.[ch]    # the document model: project/tracks/clips
    │   │   └── oe_project_format.[ch] # strict JSON v1, atomic save/load
    │   ├── app/
    │   │   ├── oe_application.[ch]# GtkApplication; startup/shutdown owner
    │   │   ├── oe_command.[ch]    # GTK-free command registry (IDs, accels)
    │   │   ├── oe_import_worker.[ch]# one decode thread; main-context results
    │   │   ├── oe_log.[ch]        # structured logging, OE_LOG_LEVEL
    │   │   ├── oe_media_cache.[ch]# raw-binary derived-media cache (XDG)
    │   │   ├── oe_media_library.[ch]# session assets: statuses, monitors
    │   │   └── oe_playback_session.[ch] # GTK-free playback clock/state machine
    │   ├── media/
    │   │   ├── oe_ffmpeg.[ch]     # FFmpeg lifecycle adapter (GError, g_once)
    │   │   ├── oe_media_jobs.[ch] # GTK-free thumbnail + waveform decode jobs
    │   │   ├── oe_media_playback.[ch] # playback decode: audio worker + video
    │   │   └── oe_probe.[ch]      # FFmpeg metadata probe (GError domain)
    │   ├── playback/
    │   │   └── oe_audio_output.[ch] # SDL3 audio adapter + push-model stream
    │   └── ui/
    │       ├── oe_main_window.[ch]  # the editor shell: panels, status bar
    │       ├── oe_media_bin.[ch]    # the media bin: rows, badges, DnD
    │       ├── oe_program_monitor.[ch] # the program monitor: frame blits
    │       ├── oe_timeline_layout.[ch] # GTK-free zoom/geometry/hit-test math
    │       ├── oe_timeline.[ch]      # the timeline widget: Cairo drawing + drags
    │       ├── oe_shell_layout.[ch] # GTK-free layout persistence (GKeyFile)
    │       ├── oe_theme.[ch]        # GtkCssProvider loader (GResource CSS)
    │       ├── obvious-edit.css     # the original dark theme
    │       └── obvious-edit.gresource.xml # CSS declared as a GResource
    ├── tests/
    │   ├── test_lifecycle.c       # 3 GLib smoke tests
    │   ├── test_commands.c        # registry integrity + dispatch paths
    │   ├── test_shell_layout.c    # layout round-trip + failure modes
    │   ├── test_probe.c           # probe metadata + error contract
    │   ├── test_media_jobs.c      # thumbnail box-fit, peaks, cache hit/miss
    │   ├── test_media_library.c   # records, observers, monitor round trip
    │   ├── test_import_worker.c   # thread → main-context completion
    │   ├── test_time.c            # rational time: reduction, rounding, rates
    │   ├── test_project.c         # model invariants, observer, media refs
    │   ├── test_project_format.c  # strict v1, round trip, atomic saves
    │   ├── test_timeline_layout.c # zoom conversions, hit-test, drag clamps
    │   ├── test_playback_clock.c  # session clock on a virtual time source
    │   ├── test_undo_stack.c      # undo/redo: inverses, rejection, depth
    │   ├── test_audio_output.c    # SDL dummy-driver adapter contract
    │   ├── fixture_media.[ch]     # runtime media fixtures (WAV/AVI/PNG)
    │   └── valgrind.supp          # GLib/GObject-only suppressions
    └── docs/
        ├── architecture.md        # system shape and layer rules
        ├── project-format.md      # reserved design space (JSON, versioned)
        ├── code-map.md            # this file
        ├── glossary.md            # NLE vocabulary
        └── learning/
            ├── phase-0.md         # guided Phase 0 walkthrough
            ├── phase-1.md         # guided Phase 1 walkthrough
            ├── phase-2.md         # guided Phase 2 walkthrough
            ├── phase-3.md         # guided Phase 3 walkthrough
            ├── phase-4.md         # guided Phase 4 walkthrough
            ├── phase-5.md         # guided Phase 5 walkthrough
            └── phase-6.md         # guided Phase 6 walkthrough
```

## File-by-file responsibilities

| File | Owns | Key entry points |
|---|---|---|
| `src/main.c` | Process lifetime | `main()` — oe_log_init, app run, exit code |
| `src/app/oe_application.c` | Lifecycle ordering, command actions, --self-check | `oe_application_new`, vfuncs |
| `src/app/oe_command.c` | Command registry: IDs, accelerators, dispatch | `oe_command_table`, `oe_command_dispatch`, `oe_command_set_reporter` |
| `src/app/oe_log.c` | Log threshold + structured emission | `oe_log_init`, `oe_log`, `oe_log_get_level` |
| `src/media/oe_ffmpeg.c` | avformat network init/teardown (thread-safe via g_once) | `oe_ffmpeg_init`, `oe_ffmpeg_shutdown` |
| `src/media/oe_probe.c` | File classification + metadata extraction | `oe_probe_file`, `oe_probe_info_clear/copy` |
| `src/media/oe_media_jobs.c` | Thumbnail + waveform decode jobs | `oe_media_job_thumbnail`, `oe_media_job_waveform` |
| `src/app/oe_media_cache.c` | Derived-media cache: keys, lookup, atomic store | `oe_media_cache_lookup`, `oe_media_cache_store` |
| `src/app/oe_media_library.c` | Session asset records, statuses, monitors | `oe_media_library_add`, `oe_media_library_relink`, `…_set_observer` |
| `src/core/oe_time.c` | Reduced rationals, frame↔µs conversions (nearest, halves away) | `oe_time_rate`, `oe_time_rate_reduce`, `oe_time_frame_to_us`, `oe_time_us_to_frame` |
| `src/core/oe_project.c` | The document model: sequence/tracks/clips, media refs, observer | `oe_project_insert_clip`, `oe_project_move_clip`, `oe_project_get_sequence` |
| `src/core/oe_project_format.c` | Strict JSON v1 load + atomic save | `oe_project_format_load`, `oe_project_format_save` |
| `src/app/oe_import_worker.c` | The decode thread: queue, cancel, dispatch | `oe_import_worker_new`, `oe_import_worker_submit`, `oe_import_worker_free` |
| `src/playback/oe_audio_output.c` | SDL audio subsystem init/quit + the push-model device stream (queue, depth, flush, pause/resume) | `oe_audio_output_init`, `oe_audio_output_open_stream`, `oe_audio_output_queue`, `oe_audio_output_shutdown` |
| `src/media/oe_media_playback.c` | Playback decode: audio decode-ahead worker (owned f32 chunks, main-context delivery) + frame-at-time BGRA video | `oe_media_playback_request_audio`, `oe_media_playback_video_open`, `oe_media_playback_video_get_frame` |
| `src/app/oe_playback_session.c` | The GTK-free playback clock: stopped/paused/playing, wall-anchor, drift accounting, clip→source mapping, injectable time source (tests install a virtual clock), events | `oe_playback_session_play`, `oe_playback_session_tick`, `oe_playback_session_seek`, `oe_playback_session_set_time_source`, `oe_playback_session_map` |
| `src/app/oe_undo_stack.c` | GTK-free command-object history: strict-LIFO records (depth 100, redo branch cleared on new edits), recorder helpers that mutate-then-record, undo/redo applied only through typed model mutators, changed-state seam, auto-pause entry point | `oe_undo_stack_undo`, `oe_undo_stack_redo`, `oe_edit_insert_clip`, `oe_undo_stack_set_changed_func` |
| `src/ui/oe_program_monitor.c` | The program monitor: owned-frame Cairo blits, empty state, missing-media hatch | `oe_program_monitor_new`, `oe_program_monitor_show_frame`, `oe_program_monitor_set_empty_state` |
| `src/ui/oe_main_window.c` | The editor shell: panels, menus, toolbar, status bar, import wiring, inspector | `oe_main_window_new` |
| `src/ui/oe_media_bin.c` | The bin panel: row projection, badges, DnD, selection | `oe_media_bin_new`, `oe_media_bin_refresh` |
| `src/ui/oe_timeline_layout.c` | GTK-free timeline math: zoom round-trips, lane mapping, edge-band hit-test, move/trim clamps | `oe_timeline_x_for_us`, `oe_timeline_hit_test`, `oe_timeline_clamp_move_position`, `oe_timeline_trim_bounds` |
| `src/ui/oe_timeline.c` | The timeline widget: observer snapshots, Cairo painting, one drag state machine → model mutators | `oe_timeline_new`, `oe_timeline_set_project`, `oe_timeline_get_selection`, `oe_timeline_zoom_in/out` |
| `src/ui/oe_shell_layout.c` | Layout struct, GKeyFile save/load | `oe_shell_layout_defaults`, `oe_shell_layout_save`, `oe_shell_layout_load` |
| `src/ui/oe_theme.c` | Theme loading from GResource | `oe_theme_init` |
| `tests/test_lifecycle.c` | Adapter + logging contracts | `/lifecycle/*`, `/log/*` |
| `tests/test_commands.c` | Registry integrity + dispatch | `/commands/*` |
| `tests/test_shell_layout.c` | Persistence round trip + fallbacks | `/shell-layout/*` |
| `tests/test_probe.c` | Probe metadata + error codes | `/probe/*` |
| `tests/test_media_jobs.c` | Decode jobs + cache round trip | `/media-jobs/*` |
| `tests/test_media_library.c` | Records, observers, monitor, relink | `/media-library/*` |
| `tests/test_import_worker.c` | Worker thread → main-context contract | `/import-worker/*` |
| `tests/test_time.c` | Rate reduction, typed rejection, rounding, identities | `/time/*` |
| `tests/test_project.c` | Ordering, overlap, observer, deep copies, media refs, trim validation | `/project/*` |
| `tests/test_project_format.c` | Strict v1 parse, round trip, atomic failure | `/format/*` |
| `tests/test_timeline_layout.c` | Pure timeline math: zoom, lanes, hit-test bands, drag clamps | `/timeline-layout/*` |
| `tests/test_playback_clock.c` | Session clock on a virtual time source: mapping, deadlines, drift, seek, end-of-sequence | `/clock/*` |
| `tests/test_undo_stack.c` | Per-op inverses, typed rejection at record/apply time, depth eviction, redo clearing, JSON round trips, auto-pause | `/undo/*` |
| `tests/test_audio_output.c` | Adapter contract on SDL's dummy driver: init, open, queue depth, pause/resume | `/audio-output/*` |
| `tests/fixture_media.c` | Runtime WAV/AVI/PNG/text fixture generator | `oe_fixture_media_create`, `oe_fixture_media_free` |

## Conventions

- Names: `oe_<module>_<verb>`, files named after the primary type.
- Every `.c` includes its own header first — header self-sufficiency is
  verified by the compiler on every build.
- GTK types come from `G_DECLARE_FINAL_TYPE`; no floating refs leak out
  of constructors (`oe_main_window_new` returns a full reference).
- No `printf` anywhere: logging goes through `oe_log` only.
- Command logic and persistence stay GTK-free: if a module can be
  unit-tested without a display, it must not include gtk.h.
