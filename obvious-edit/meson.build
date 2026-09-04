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
    'src/core/oe_project.c',
    'src/core/oe_project_format.c',
    'src/app/oe_application.c',
    'src/app/oe_command.c',
    'src/app/oe_log.c',
    'src/app/oe_media_cache.c',
    'src/app/oe_media_library.c',
    'src/app/oe_import_worker.c',
    'src/media/oe_ffmpeg.c',
    'src/media/oe_probe.c',
    'src/media/oe_media_jobs.c',
    'src/playback/oe_audio_output.c',
    'src/ui/oe_main_window.c',
    'src/ui/oe_media_bin.c',
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
    'src/core/oe_project.c',
    'src/core/oe_time.c',
  ],
  c_args: oe_log_cargs,
  dependencies: [glib_dep, gobject_dep],
)

test('timeline-layout', test_timeline_layout)
