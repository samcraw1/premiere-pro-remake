# Code map (Phases 1–7)

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
    │   │   ├── oe_export.[ch]     # GTK-free MP4 export: x264/aac, atomic
    │   │   ├── oe_ffmpeg.[ch]     # FFmpeg lifecycle adapter (GError, g_once)
    │   │   ├── oe_media_jobs.[ch] # GTK-free thumbnail + waveform decode jobs
    │   │   ├── oe_media_playback.[ch] # playback decode: audio worker + video
    │   │   ├── oe_probe.[ch]      # FFmpeg metadata probe (GError domain)
    │   │   └── oe_render.[ch]     # GTK-free frame-at-time render seam
    │   ├── playback/
    │   │   └── oe_audio_output.[ch] # SDL3 audio adapter + push-model stream
    │   └── ui/
    │       ├── oe_main_window.[ch]  # the editor shell: panels, status bar
    │       ├── oe_media_bin.[ch]    # the media bin: rows, badges, DnD
    │       ├── oe_meter.[ch]        # Cairo peak meter widget (GTK shell)
    │       ├── oe_meter_math.[ch]   # GTK-free meter decay + bar geometry
    │       ├── oe_program_monitor.[ch] # the program monitor: frame blits
    │       ├── oe_timeline_layout.[ch] # GTK-free zoom/geometry/hit-test/snap math
    │       ├── oe_timeline.[ch]      # the timeline widget: Cairo drawing + drags
    │       ├── oe_shell_layout.[ch] # GTK-free layout persistence (GKeyFile)
    │       ├── oe_theme.[ch]        # GtkCssProvider loader (GResource CSS)
    │       ├── obvious-edit.css     # the original dark theme
    │       └── obvious-edit.gresource.xml # CSS declared as a GResource
    ├── tests/
    │   ├── test_lifecycle.c       # 3 GLib smoke tests
    │   ├── test_commands.c        # registry integrity + dispatch paths
    │   ├── test_export.c          # export: grid, parity, container, mixdown, atomicity
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
    │   ├── test_snap_ripple.c     # snapping + ripple: targets, bands, composite records
    │   ├── test_wave_b.c          # keyframes, fades, transitions, strictness (Wave B)
    │   ├── test_audio_output.c    # SDL dummy-driver adapter contract
    │   ├── test_audio_tools.c     # buffer peaks, factor chain, meter decay/geometry
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
            ├── phase-6.md         # guided Phase 6 walkthrough
            ├── phase-7.md         # guided Phase 7 walkthrough
            ├── phase-8.md         # guided Phase 8 walkthrough
            └── phase-9.md         # guided Phase 9 walkthrough
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
| `src/media/oe_render.c` | GTK-free frame-at-time render seam: covering-clip collection (keyframe resolution, transition ramp weights), per-source sequential decoder cache, layered compositor (ascending track order: crop → scale → rotate → translate → straight integer src-over; transition pairs blended `(A*(255-w)+B*w)/255` per channel with w=0/255 degenerating to the cut), single-default-transform fast path, centered even box-fit | `oe_render_blend_channel`, `oe_render_session_new`, `oe_render_session_frame_at`, `oe_render_frame_at` |
| `src/media/oe_export.c` | Synchronous GTK-free MP4 export: integer frame grid over the render seam, x264/AAC encode, additive 48 kHz stereo mixdown (per-AVFrame shared fade envelope folded into the integer factor chain with the mute/solo matrix, single final hard clamp), custom-AVIO temp + fsync + rename atomicity, per-frame cancellation | `oe_export_frame_count`, `oe_export_frame_to_us`, `oe_export_run` |
| `src/app/oe_media_cache.c` | Derived-media cache: keys, lookup, atomic store | `oe_media_cache_lookup`, `oe_media_cache_store` |
| `src/app/oe_media_library.c` | Session asset records, statuses, monitors | `oe_media_library_add`, `oe_media_library_relink`, `…_set_observer` |
| `src/core/oe_time.c` | Reduced rationals, frame↔µs conversions (nearest, halves away) | `oe_time_rate`, `oe_time_rate_reduce`, `oe_time_frame_to_us`, `oe_time_us_to_frame` |
| `src/core/oe_fades.c` | Shared GTK-free/FFmpeg-free audio fade envelope: linear integer ramp on a 0–1024 scale, one rounding per side, consumed by the mixdown and the playback chunk path | `oe_fade_gain`, `OE_FADE_SCALE` |
| `src/core/oe_audio_factor.c` | Shared GTK-free/FFmpeg-free integer factor chain: fade x clip gain/pan x track volume/pan per channel on the unity-1024 scale, linear pan pair with center unity, mute/lose-solo zeroing, one 64-bit product and one final rounding — consumed by the export mixdown, by the playback mixer from Wave B | `oe_audio_factor`, `oe_audio_audible`, `OE_AUDIO_UNITY` |
| `src/core/oe_keyframes.c` | Per-property keyframe stores: closed property set, domain validation, sorted insert/remove, deep copy, equality, linear interpolation with one final rounding and endpoint clamping, static-value degradation | `oe_keyframes_sample`, `oe_keyframes_insert`, `oe_keyframes_remove`, `oe_clip_visual_resolve` |
| `src/core/oe_project.c` | The document model: sequence/tracks/clips with owned `OeClipVisual` and `OeClipAudio` (identity defaults, deep copies), media refs, observer, validated visual/transition/keyframe/audio mutators, `OeTransition` boundary objects | `oe_project_insert_clip`, `oe_project_move_clip`, `oe_project_set_clip_visual`, `oe_project_set_clip_keyframe`, `oe_project_set_clip_audio`, `oe_project_set_track_audio`, `oe_project_add_transition`, `oe_project_get_sequence` |
| `src/core/oe_project_format.c` | Strict JSON v1 load + atomic save; clip `visual` (always written, identity backfill), `keyframes` and track `transitions` members (always written, absence means none, closed member lists), clip and audio-track `audio` members (always written, identity backfill, video tracks carry no member) | `oe_project_format_load`, `oe_project_format_save` |
| `src/app/oe_import_worker.c` | The decode thread: queue, cancel, dispatch | `oe_import_worker_new`, `oe_import_worker_submit`, `oe_import_worker_free` |
| `src/playback/oe_audio_output.c` | SDL audio subsystem init/quit + the push-model device stream (queue, depth, flush, pause/resume) | `oe_audio_output_init`, `oe_audio_output_open_stream`, `oe_audio_output_queue`, `oe_audio_output_shutdown` |
| `src/media/oe_media_playback.c` | Playback decode: audio decode-ahead worker (owned f32 chunks, main-context delivery, source-time labels anchored to the chunk buffer across decoder frames) + frame-at-time BGRA video | `oe_media_playback_request_audio`, `oe_media_playback_video_open`, `oe_media_playback_video_get_frame` |
| `src/app/oe_playback_session.c` | The GTK-free playback clock: stopped/paused/playing, wall-anchor, drift accounting, clip→source mapping, shared-seam monitor rendering through an owned render session (same decoder cache as export, same-frame dedup, paused repaint on project notifications), injectable time source (tests install a virtual clock), events; Wave B: the multi-track mix window (all audible audio tracks summed in track-array order through the shared factor chain, gaps silent, one interleaved f32 queue feed), per-chunk peak/mix observers (`set_meter_func`, `set_mix_func`), coverage-gated lookahead feeding | `oe_playback_session_play`, `oe_playback_session_tick`, `oe_playback_session_seek`, `oe_playback_session_set_time_source`, `oe_playback_session_map`, `oe_playback_session_set_meter_func`, `oe_playback_session_set_mix_func` |
| `src/app/oe_undo_stack.c` | GTK-free command-object history: strict-LIFO records (depth 100, redo branch cleared on new edits), recorder helpers that mutate-then-record, one-record-per-stroke visual records (`OE_UNDO_OP_VISUAL` with stroke baseline + final visual) and audio records (`OE_UNDO_OP_CLIP_AUDIO` clip-indexed, `OE_UNDO_OP_TRACK_AUDIO` track-indexed — stroke baselines, zero-delta suppression), composite ripple-delete records (primary copy + suffix pre/post positions and indices), undo/redo applied only through typed model mutators, changed-state seam, auto-pause entry point | `oe_undo_stack_undo`, `oe_undo_stack_redo`, `oe_edit_insert_clip`, `oe_edit_set_clip_visual`, `oe_edit_ripple_remove_clip`, `oe_undo_stack_set_changed_func` |
| `src/ui/oe_program_monitor.c` | The program monitor: owned-frame Cairo blits, empty state, missing-media hatch | `oe_program_monitor_new`, `oe_program_monitor_show_frame`, `oe_program_monitor_set_empty_state` |
| `src/ui/oe_meter.c` | Cairo peak meter widget: a thin GTK shell over `oe_meter_math` — main-context peak updates, notify-driven repaints, explicit release-to-silence on pause/stop/seek (no timers while paused) | `oe_meter_new`, `oe_meter_set_peaks`, `oe_meter_release` |
| `src/ui/oe_meter_math.c` | GTK-free meter math: per-update peak-hold with a documented decay rule and Cairo bar geometry (clamped edges, minimum readable width — the transition-band precedent) | `oe_meter_math_apply`, `oe_meter_math_bar` |
| `src/ui/oe_main_window.c` | The editor shell: panels, menus, toolbar, status bar, import wiring, inspector (third stack page: clip visual properties with preview-then-commit editing) | `oe_main_window_new` |
| `src/ui/oe_media_bin.c` | The bin panel: row projection, badges, DnD, selection | `oe_media_bin_new`, `oe_media_bin_refresh` |
| `src/ui/oe_timeline_layout.c` | GTK-free timeline math: zoom round-trips, lane mapping, edge-band hit-test, move/trim clamps, the pure snap decision (`OeSnapContext`: px-scaled threshold, edges/playhead/zero/frame-grid targets, nearest-wins earlier-tie-break) | `oe_timeline_x_for_us`, `oe_timeline_hit_test`, `oe_timeline_clamp_move_position`, `oe_timeline_trim_bounds`, `oe_timeline_snap_time` |
| `src/ui/oe_timeline.c` | The timeline widget: observer snapshots, Cairo painting, one drag state machine → model mutators, snap-then-clamp drag previews, snapping session flag (`_set/get_snapping`) | `oe_timeline_new`, `oe_timeline_set_project`, `oe_timeline_get_selection`, `oe_timeline_set_snapping`, `oe_timeline_zoom_in/out` |
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
| `tests/test_project.c` | Ordering, overlap, observer, deep copies, media refs, trim validation, visual identity defaults, validated visual mutation + typed rejection, audio state identity/copy/mutation (clip and audio-track) | `/project/*` |
| `tests/test_project_format.c` | Strict v1 parse, round trip, atomic failure, audio members (byte-identical round trip, identity backfill, strict values, video-track rejection) | `/format/*` |
| `tests/test_timeline_layout.c` | Pure timeline math: zoom, lanes, hit-test bands, drag clamps | `/timeline-layout/*` |
| `tests/test_playback_clock.c` | Session clock on a virtual time source: mapping, deadlines, drift, seek, end-of-sequence | `/clock/*` |
| `tests/test_undo_stack.c` | Per-op inverses, visual stroke records (one record per stroke, stroke-baseline restore, zero-delta suppression), audio stroke records for both ops (clip-indexed and track-indexed payloads), typed rejection at record/apply time, depth eviction, redo clearing, JSON round trips, auto-pause | `/undo/*` |
| `tests/test_snap_ripple.c` | Pure snap decision (targets, band boundaries, tie-break, zoom scaling, disabled pass-through, snap-then-clamp) + composite ripple records (first/middle/last deletes, typed rejection, JSON round trips, depth, redo clearing, auto-pause) | `/snap-ripple/*` |
| `tests/test_audio_output.c` | Adapter contract on SDL's dummy driver: init, open, queue depth, pause/resume | `/audio-output/*` |
| `tests/test_audio_tools.c` | GTK-free audio-tools units: interleaved-f32 per-channel peak extraction (empty, NULL, channel clamp), the shared factor chain applied to a buffer (scale, hard pan, clamp), meter decay rule, meter bar geometry | `/audio-tools/*` |
| `tests/test_export.c` | Frame-grid math, straight-cut render parity vs the preview seam, compositor equivalence (blend-unit ±1, two-layer seam, two-layer export decode-back parity at |Δ| ≤ 8 block means), MP4 container truth via probe, decoded video/audio round trip (color/amplitude classes), additive two-track mixdown, cancellation cleanup, atomic-failure byte identity, Wave B transition blend (degenerate edges + midpoint), mixdown fade ratio bands, two-layer + transition + fade parity, audio factor chains (per-channel pan dominance, gain halving, mute/solo matrix), Wave B two-track playback/export parity (real mixer capture vs decoded mixdown, per-channel ≤ 0.02) | `/export/*` |
| `tests/test_wave_b.c` | Keyframe contract (single rounding, clamping, degradation, per-property resolution, undo record), fade envelope endpoints, transition windows/mutators, GTK-free band/snap-edge layout, ripple re-anchor replay, persistence round trip + strictness mutations | `/wave-b/*` |
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
