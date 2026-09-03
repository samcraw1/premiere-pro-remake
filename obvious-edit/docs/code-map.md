# Code map (Phases 1 + 2)

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
    │   ├── app/
    │   │   ├── oe_application.[ch]# GtkApplication; startup/shutdown owner
    │   │   ├── oe_command.[ch]    # GTK-free command registry (IDs, accels)
    │   │   ├── oe_import_worker.[ch]# one decode thread; main-context results
    │   │   ├── oe_log.[ch]        # structured logging, OE_LOG_LEVEL
    │   │   ├── oe_media_cache.[ch]# raw-binary derived-media cache (XDG)
    │   │   └── oe_media_library.[ch]# session assets: statuses, monitors
    │   ├── media/
    │   │   ├── oe_ffmpeg.[ch]     # FFmpeg lifecycle adapter (GError, g_once)
    │   │   ├── oe_media_jobs.[ch] # GTK-free thumbnail + waveform decode jobs
    │   │   └── oe_probe.[ch]      # FFmpeg metadata probe (GError domain)
    │   ├── playback/
    │   │   └── oe_audio_output.[ch] # SDL3 audio lifecycle adapter (GError)
    │   └── ui/
    │       ├── oe_main_window.[ch]  # the editor shell: panels, status bar
    │       ├── oe_media_bin.[ch]    # the media bin: rows, badges, DnD
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
    │   ├── fixture_media.[ch]     # runtime media fixtures (WAV/AVI/PNG)
    │   └── valgrind.supp          # GLib/GObject-only suppressions
    └── docs/
        ├── architecture.md        # system shape and layer rules
        ├── project-format.md      # reserved design space (JSON, versioned)
        ├── code-map.md            # this file
        ├── glossary.md            # NLE vocabulary
        └── learning/
            ├── phase-0.md         # guided Phase 0 walkthrough
            └── phase-1.md         # guided Phase 1 walkthrough
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
| `src/app/oe_import_worker.c` | The decode thread: queue, cancel, dispatch | `oe_import_worker_new`, `oe_import_worker_submit`, `oe_import_worker_free` |
| `src/playback/oe_audio_output.c` | SDL audio subsystem init/quit | `oe_audio_output_init`, `oe_audio_output_shutdown` |
| `src/ui/oe_main_window.c` | The editor shell: panels, menus, toolbar, status bar, import wiring, inspector | `oe_main_window_new` |
| `src/ui/oe_media_bin.c` | The bin panel: row projection, badges, DnD, selection | `oe_media_bin_new`, `oe_media_bin_refresh` |
| `src/ui/oe_shell_layout.c` | Layout struct, GKeyFile save/load | `oe_shell_layout_defaults`, `oe_shell_layout_save`, `oe_shell_layout_load` |
| `src/ui/oe_theme.c` | Theme loading from GResource | `oe_theme_init` |
| `tests/test_lifecycle.c` | Adapter + logging contracts | `/lifecycle/*`, `/log/*` |
| `tests/test_commands.c` | Registry integrity + dispatch | `/commands/*` |
| `tests/test_shell_layout.c` | Persistence round trip + fallbacks | `/shell-layout/*` |
| `tests/test_probe.c` | Probe metadata + error codes | `/probe/*` |
| `tests/test_media_jobs.c` | Decode jobs + cache round trip | `/media-jobs/*` |
| `tests/test_media_library.c` | Records, observers, monitor, relink | `/media-library/*` |
| `tests/test_import_worker.c` | Worker thread → main-context contract | `/import-worker/*` |
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
