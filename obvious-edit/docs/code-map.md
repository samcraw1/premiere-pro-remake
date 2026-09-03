# Code map (Phase 0)

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
    │   │   └── oe_log.[ch]        # structured logging, OE_LOG_LEVEL
    │   ├── media/
    │   │   └── oe_ffmpeg.[ch]     # FFmpeg lifecycle adapter (GError)
    │   ├── playback/
    │   │   └── oe_audio_output.[ch] # SDL3 audio lifecycle adapter (GError)
    │   └── ui/
    │       └── oe_main_window.[ch]  # OeMainWindow, titled "Obvious Edit"
    ├── tests/
    │   ├── test_lifecycle.c       # 3 GLib smoke tests
    │   └── valgrind.supp          # GLib/GObject-only suppressions
    └── docs/
        ├── architecture.md        # system shape and layer rules
        ├── project-format.md      # reserved design space (JSON, versioned)
        ├── code-map.md            # this file
        ├── glossary.md            # NLE vocabulary
        └── learning/
            └── phase-0.md         # guided Phase 0 walkthrough
```

## File-by-file responsibilities

| File | Owns | Key entry points |
|---|---|---|
| `src/main.c` | Process lifetime | `main()` — oe_log_init, app run, exit code |
| `src/app/oe_application.c` | Lifecycle ordering, --self-check | `oe_application_new`, vfuncs |
| `src/app/oe_log.c` | Log threshold + structured emission | `oe_log_init`, `oe_log`, `oe_log_get_level` |
| `src/media/oe_ffmpeg.c` | avformat network init/teardown | `oe_ffmpeg_init`, `oe_ffmpeg_shutdown` |
| `src/playback/oe_audio_output.c` | SDL audio subsystem init/quit | `oe_audio_output_init`, `oe_audio_output_shutdown` |
| `src/ui/oe_main_window.c` | The main window | `oe_main_window_new` |
| `tests/test_lifecycle.c` | Adapter + logging contracts | `/lifecycle/*`, `/log/*` |

## Conventions

- Names: `oe_<module>_<verb>`, files named after the primary type.
- Every `.c` includes its own header first — header self-sufficiency is
  verified by the compiler on every build.
- GTK types come from `G_DECLARE_FINAL_TYPE`; no floating refs leak out
  of constructors (`oe_main_window_new` returns a full reference).
- No `printf` anywhere: logging goes through `oe_log` only.
