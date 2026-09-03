project('obvious-edit', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.3.0',
  default_options: [
    'c_std=c17',
    'warning_level=2',
    'werror=true',
  ])

# Dependency floors are policy: configure fails on anything older, so a
# developer never chases a build break that is really a stale toolchain.
gtk4_dep = dependency('gtk4', version: '>= 4.10')
glib_dep = dependency('glib-2.0', version: '>= 2.74')
gobject_dep = dependency('gobject-2.0', version: '>= 2.74')
json_glib_dep = dependency('json-glib-1.0', version: '>= 1.6')
avformat_dep = dependency('libavformat', version: '>= 60')
avcodec_dep = dependency('libavcodec', version: '>= 60')
avutil_dep = dependency('libavutil', version: '>= 58')
avfilter_dep = dependency('libavfilter', version: '>= 9')
swscale_dep = dependency('libswscale', version: '>= 7')
swresample_dep = dependency('libswresample', version: '>= 4')
sdl3_dep = dependency('sdl3', version: '>= 3.2')

# One structured-logging domain for the whole project.
oe_log_cargs = ['-DG_LOG_DOMAIN="oe"']

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
]

executable('obvious-edit',
  [
    'src/main.c',
    'src/app/oe_application.c',
    'src/app/oe_log.c',
    'src/media/oe_ffmpeg.c',
    'src/playback/oe_audio_output.c',
    'src/ui/oe_main_window.c',
  ],
  c_args: oe_log_cargs,
  dependencies: oe_deps,
  install: true,
)

# The smoke tests exercise the adapters without linking GTK.
test_deps = [
  glib_dep,
  gobject_dep,
  avformat_dep,
  avcodec_dep,
  avutil_dep,
  avfilter_dep,
  swscale_dep,
  swresample_dep,
  sdl3_dep,
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


