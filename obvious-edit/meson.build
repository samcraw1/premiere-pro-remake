project('obvious-edit', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.3.0',
  default_options: [
    'c_std=c17',
    'warning_level=2',
    'werror=true',
  ])

gnome = import('gnome')

# The theme stylesheet is compiled into the binary, so the shell can never
# come up unstyled from a mis-installed data file.
oe_resources = gnome.compile_resources('oe_resources',
  'src/ui/obvious-edit.gresource.xml',
  source_dir: 'src/ui',
)

# Dependency floors are policy: configure fails on anything older, so a
# developer never chases a build break that is really a stale toolchain.
gtk4_dep = dependency('gtk4', version: '>= 4.10')
glib_dep = dependency('glib-2.0', version: '>= 2.74')
gobject_dep = dependency('gobject-2.0', version: '>= 2.74')
json_glib_dep = dependency('json-glib-1.0', version: '>= 1.6')
gio_dep = dependency('gio-2.0', version: '>= 2.74')
avformat_dep = dependency('libavformat', version: '>= 60')
avcodec_dep = dependency('libavcodec', version: '>= 60')
avutil_dep = dependency('libavutil', version: '>= 58')
avfilter_dep = dependency('libavfilter', version: '>= 9')
swscale_dep = dependency('libswscale', version: '>= 7')
swresample_dep = dependency('libswresample', version: '>= 4')
sdl3_dep = dependency('sdl3', version: '>= 3.2')

# One structured-logging domain for the whole project.
oe_log_cargs = ['-DG_LOG_DOMAIN="oe"']

# The fixture generator synthesises sine tones: libm for sin/cos.
math_dep = meson.get_compiler('c').find_library('m')

oe_deps = [
  gtk4_dep,
  glib_dep,
  gobject_dep,
  json_glib_dep,
  avformat_dep,
  avcodec_dep,
  avutil_dep,
  avfilter_dep,
  swscale_dep,
  swresample_dep,
  sdl3_dep,
  math_dep,
]

executable('obvious-edit',
  [
    oe_resources,
    'src/main.c',
    'src/core/oe_time.c',
    'src/core/oe_fades.c',
    'src/core/oe_keyframes.c',
    'src/core/oe_audio_factor.c',
    'src/core/oe_audio_buffer.c',
    'src/core/oe_project.c',
    'src/core/oe_project_format.c',
    'src/app/oe_application.c',
    'src/app/oe_command.c',
    'src/app/oe_log.c',
    'src/app/oe_undo_stack.c',
    'src/app/oe_media_cache.c',
    'src/app/oe_media_library.c',
    'src/app/oe_import_worker.c',
    'src/app/oe_playback_session.c',
    'src/media/oe_ffmpeg.c',
    'src/media/oe_probe.c',
    'src/media/oe_media_jobs.c',
    'src/media/oe_media_playback.c',
    'src/media/oe_render.c',
    'src/media/oe_export.c',
    'src/playback/oe_audio_output.c',
    'src/ui/oe_main_window.c',
    'src/ui/oe_media_bin.c',
    'src/ui/oe_program_monitor.c',
    'src/ui/oe_timeline_layout.c',
    'src/ui/oe_timeline.c',
    'src/ui/oe_shell_layout.c',
    'src/ui/oe_theme.c',
  ],
  c_args: oe_log_cargs,
  dependencies: oe_deps,
  install: true,
)

# The smoke tests exercise the adapters without linking GTK.
test_deps = [
  glib_dep,
  gio_dep,
  gobject_dep,
  avformat_dep,
  avcodec_dep,
  avutil_dep,
  avfilter_dep,
  swscale_dep,
  swresample_dep,
  sdl3_dep,
  math_dep,
]

test_lifecycle = executable('test_lifecycle',
  [
    'tests/test_lifecycle.c',
    'src/app/oe_log.c',
    'src/media/oe_ffmpeg.c',
    'src/playback/oe_audio_output.c',
  ],
  c_args: oe_log_cargs,
  dependencies: test_deps,
)

test('lifecycle', test_lifecycle,
  env: ['SDL_AUDIODRIVER=dummy'])

# Command registry: GTK-free by design, so the table and dispatch paths are
# tested without a display (same executable() + test() pattern as above).
test_commands = executable('test_commands',
  [
    'tests/test_commands.c',
    'src/app/oe_command.c',
    'src/app/oe_log.c',
  ],
  c_args: oe_log_cargs,
  dependencies: [glib_dep, gobject_dep],
)

test('commands', test_commands)

# Shell layout persistence: GLib/GKeyFile only, no GTK.
test_shell_layout = executable('test_shell_layout',
  [
    'tests/test_shell_layout.c',
    'src/ui/oe_shell_layout.c',
    'src/app/oe_log.c',
  ],
  c_args: oe_log_cargs,
  dependencies: [glib_dep, gobject_dep],
)

test('shell-layout', test_shell_layout)

# Probe tests are the contract for the media metadata layer (GTK-free).
test_probe = executable('test_probe',
  [
    'tests/test_probe.c',
    'tests/fixture_media.c',
    'src/media/oe_probe.c',
    'src/app/oe_log.c',
  ],
  c_args: oe_log_cargs,
  dependencies: test_deps,
)

test('probe', test_probe)

# Decode jobs (thumbnail, waveform) run GTK-free with cancellation; the
# cache round-trip test links the app-layer cache module (glib-only).
test_media_jobs = executable('test_media_jobs',
  [
    'tests/test_media_jobs.c',
    'tests/fixture_media.c',
    'src/media/oe_probe.c',
    'src/media/oe_media_jobs.c',
    'src/app/oe_log.c',
    'src/app/oe_media_cache.c',
  ],
  c_args: oe_log_cargs,
  dependencies: test_deps,
)

test('media-jobs', test_media_jobs)

# Session asset records: statuses, observers, and a real GFileMonitor
# round trip, all GTK-free.
test_media_library = executable('test_media_library',
  [
    'tests/test_media_library.c',
    'src/app/oe_log.c',
    'src/app/oe_media_library.c',
    'src/media/oe_probe.c',
  ],
  c_args: oe_log_cargs,
  dependencies: test_deps,
)

test('media-library', test_media_library)

# Import worker: one decode thread with main-context completion; uses
# the media layer for probing and the cache for hit/miss behavior.
test_import_worker = executable('test_import_worker',
  [
    'tests/test_import_worker.c',
    'tests/fixture_media.c',
    'src/media/oe_ffmpeg.c',
    'src/media/oe_probe.c',
    'src/media/oe_media_jobs.c',
    'src/app/oe_log.c',
    'src/app/oe_media_cache.c',
    'src/app/oe_import_worker.c',
  ],
  c_args: oe_log_cargs,
  dependencies: test_deps,
)

test('import-worker', test_import_worker)

# Rational time: reduction, rounding, and conversions (GLib only).
test_time = executable('test_time',
  [
    'tests/test_time.c',
    'src/core/oe_time.c',
  ],
  c_args: oe_log_cargs,
  dependencies: [glib_dep, gobject_dep],
)

test('time', test_time)

# Project model: ordering, overlap rejection, observer, deep copies —
# links the core model only (GLib/GObject, no GTK).
test_project = executable('test_project',
  [
    'tests/test_project.c',
    'src/core/oe_fades.c',
    'src/core/oe_audio_factor.c',
    'src/core/oe_audio_buffer.c',
    'src/core/oe_keyframes.c',
    'src/core/oe_project.c',
    'src/core/oe_time.c',
  ],
  c_args: oe_log_cargs,
  dependencies: [glib_dep, gobject_dep],
)

test('project', test_project)

# Project format: strict v1 parse, integer-only serialization, and the
# atomic-save contract. json-glib is in oe_deps but not test_deps, so
# it is an explicit dependency of this suite (meson.build:80-92).
test_project_format = executable('test_project_format',
  [
    'tests/test_project_format.c',
    'src/core/oe_fades.c',
    'src/core/oe_keyframes.c',
    'src/core/oe_project.c',
    'src/core/oe_project_format.c',
    'src/core/oe_time.c',
  ],
  c_args: oe_log_cargs,
  dependencies: [glib_dep, gobject_dep, json_glib_dep],
)

test('project-format', test_project_format)

# Timeline geometry: pure math over project-model copies — GTK-free,
# so the layout suite links the core model and the layout module only
# (GLib/GObject; no math library needed, every conversion is
# multiply/divide).
test_timeline_layout = executable('test_timeline_layout',
  [
    'tests/test_timeline_layout.c',
    'src/ui/oe_timeline_layout.c',
    'src/core/oe_fades.c',
    'src/core/oe_keyframes.c',
    'src/core/oe_project.c',
    'src/core/oe_time.c',
  ],
  c_args: oe_log_cargs,
  dependencies: [glib_dep, gobject_dep],
)

test('timeline-layout', test_timeline_layout)

# Playback clock: the GTK-free session over the project model — deadline
# math, clip→source mapping, transitions, pause/resume drift accounting,
# seek, and events, all headless. Runs on SDL's dummy driver so the
# audio adapter opens without hardware; decode is exercised only
# through its failure path (missing files) to stay deterministic.
test_playback_clock = executable('test_playback_clock',
  [
    'tests/test_playback_clock.c',
    'src/app/oe_playback_session.c',
    'src/core/oe_audio_factor.c',
    'src/core/oe_audio_buffer.c',
    'src/media/oe_media_playback.c',
    'src/media/oe_render.c',
    'src/playback/oe_audio_output.c',
    'src/core/oe_fades.c',
    'src/core/oe_keyframes.c',
    'src/core/oe_project.c',
    'src/core/oe_time.c',
    'src/app/oe_log.c',
  ],
  c_args: oe_log_cargs,
  dependencies: test_deps,
)

test('playback-clock', test_playback_clock,
  timeout: 300, # generous headroom for slowed environments (valgrind, sanitizers)
  env: ['SDL_AUDIODRIVER=dummy'])

# Audio output adapter: the SDL3 push-model stream contract on the
# dummy driver — open, is_dummy reporting, queue-depth accounting,
# flush, device pause/resume, and teardown symmetry.
test_audio_output = executable('test_audio_output',
  [
    'tests/test_audio_output.c',
    'src/playback/oe_audio_output.c',
    'src/app/oe_log.c',
  ],
  c_args: oe_log_cargs,
  dependencies: [glib_dep, gobject_dep, sdl3_dep],
)

test('audio-output', test_audio_output,
  env: ['SDL_AUDIODRIVER=dummy'])

# Undo/redo history: the command-object stack over the project model —
# per-op inverse correctness, record/apply-time typed rejections, depth
# eviction, redo invalidation, JSON v1 round-trips, and the auto-pause
# interplay on a virtual clock. Links the stack, the model, the
# persistence module (json-glib is an explicit dependency, like the
# project-format suite), and the playback session; no GTK. The session
# opens SDL's dummy driver, hence the env.
test_undo_stack = executable('test_undo_stack',
  [
    'tests/test_undo_stack.c',
    'src/app/oe_undo_stack.c',
    'src/app/oe_playback_session.c',
    'src/core/oe_audio_factor.c',
    'src/core/oe_audio_buffer.c',
    'src/media/oe_media_playback.c',
    'src/media/oe_render.c',
    'src/playback/oe_audio_output.c',
    'src/core/oe_fades.c',
    'src/core/oe_keyframes.c',
    'src/core/oe_project.c',
    'src/core/oe_project_format.c',
    'src/core/oe_time.c',
    'src/app/oe_log.c',
  ],
  c_args: oe_log_cargs,
  dependencies: test_deps + [json_glib_dep],
)

test('undo-stack', test_undo_stack,
  timeout: 300, # generous headroom for slowed environments (valgrind, sanitizers)
  env: ['SDL_AUDIODRIVER=dummy'])

# Phase 7 snapping and the composite ripple record: the pure snap
# decision (targets, px-scaled threshold, tie-break, zoom scaling, the
# disabled pass-through, and the snap-then-clamp preview pipeline),
# plus the ripple-delete recorder — one composite record per action,
# suffix shifts, JSON v1 round-trips, depth accounting, redo-branch
# invalidation, and the record-vs-history auto-pause split on a
# virtual clock. Links the layout core, the model, the stack, the
# persistence module, and the playback session; no GTK. The auto-pause
# case opens SDL's dummy driver, hence the env.
test_snap_ripple = executable('test_snap_ripple',
  [
    'tests/test_snap_ripple.c',
    'src/ui/oe_timeline_layout.c',
    'src/app/oe_undo_stack.c',
    'src/app/oe_playback_session.c',
    'src/core/oe_audio_factor.c',
    'src/core/oe_audio_buffer.c',
    'src/media/oe_media_playback.c',
    'src/media/oe_render.c',
    'src/playback/oe_audio_output.c',
    'src/core/oe_fades.c',
    'src/core/oe_keyframes.c',
    'src/core/oe_project.c',
    'src/core/oe_project_format.c',
    'src/core/oe_time.c',
    'src/app/oe_log.c',
  ],
  c_args: oe_log_cargs,
  dependencies: test_deps + [json_glib_dep],
)

# Phase 9 Wave B units: the keyframe interpolation contract, the
# shared fade envelope, transition windows/mutators, timeline band
# and snap geometry, ripple re-anchor replay, keyframe undo records,
# and strict keyframes/transitions persistence. Links the layout
# core, the model, the stack, and the persistence module; no GTK,
# no media fixtures — compositor blends and decode-back equivalence
# live in the export suite.
test_wave_b = executable('test_wave_b',
  [
    'tests/test_wave_b.c',
    'src/ui/oe_timeline_layout.c',
    'src/app/oe_undo_stack.c',
    'src/app/oe_playback_session.c',
    'src/core/oe_audio_factor.c',
    'src/core/oe_audio_buffer.c',
    'src/media/oe_media_playback.c',
    'src/media/oe_render.c',
    'src/playback/oe_audio_output.c',
    'src/core/oe_fades.c',
    'src/core/oe_keyframes.c',
    'src/core/oe_project.c',
    'src/core/oe_project_format.c',
    'src/core/oe_time.c',
    'src/app/oe_log.c',
  ],
  c_args: oe_log_cargs,
  dependencies: test_deps + [json_glib_dep],
)

test('wave-b', test_wave_b,
  timeout: 300, # generous headroom for slowed environments (valgrind, sanitizers)
  env: ['SDL_AUDIODRIVER=dummy'])


test('snap-ripple', test_snap_ripple,
  timeout: 300, # generous headroom for slowed environments (valgrind, sanitizers)
  env: ['SDL_AUDIODRIVER=dummy'])

# Export job: render-seam parity, container truth, content round-trip,
# mixdown sums, cancellation, and the atomic-failure contract — GTK-free,
# synthetic in-process fixtures only, structural assertions.
test_export = executable('test_export',
  [
    'tests/test_export.c',
    'tests/fixture_media.c',
    'src/media/oe_render.c',
    'src/media/oe_export.c',
    'src/media/oe_probe.c',
    'src/media/oe_media_playback.c',
    'src/app/oe_playback_session.c',
    'src/core/oe_audio_factor.c',
    'src/core/oe_audio_buffer.c',
    'src/playback/oe_audio_output.c',
    'src/core/oe_fades.c',
    'src/core/oe_keyframes.c',
    'src/core/oe_project.c',
    'src/core/oe_time.c',
    'src/app/oe_log.c',
  ],
  c_args: oe_log_cargs,
  dependencies: test_deps,
)

test('export', test_export,
  timeout: 300, # generous headroom for slowed environments (valgrind, sanitizers)
  env: ['SDL_AUDIODRIVER=dummy'])
