# Obvious Edit

A native GTK4 video editor for Linux, written in C17. Phase 0 is the
scaffold and proof of life: build tooling, resource lifecycle adapters,
structured logging, smoke tests, and a headless window verification.

## Requirements

Debian 13 (trixie) with the following toolchain. The single install line:
    sudo apt-get update


    sudo apt-get install -y build-essential meson ninja-build clang-format valgrind xvfb dbus x11-utils libgtk-4-dev libglib2.0-dev libjson-glib-dev libavformat-dev libavcodec-dev libavutil-dev libavfilter-dev libswscale-dev libswresample-dev libsdl3-dev pkg-config

### Pinned versions (dpkg)

Verified on Debian 13 at Phase 0 kickoff. Meson enforces the pkg-config
floors at configure time; these dpkg versions are the exact reference
environment the verification gates were run against.

| Package | Version |
|---|---|
| build-essential | 12.12 |
| meson | 1.7.0-1 |
| ninja-build | 1.12.1-1 |
| clang-format | 1:19.0-63 |
| valgrind | 1:3.24.0-3 |
| xvfb | 2:21.1.16-1.3+deb13u3 |
| dbus | 1.16.2-2 |
| x11-utils | 7.7+7 |
| libgtk-4-dev | 4.18.6+ds-2 |
| libglib2.0-dev | 2.84.4-3~deb13u3 |
| libjson-glib-dev | 1.10.6+ds-2 |
| libavformat-dev | 7:7.1.5-0+deb13u1 |
| libavcodec-dev | 7:7.1.5-0+deb13u1 |
| libavutil-dev | 7:7.1.5-0+deb13u1 |
| libavfilter-dev | 7:7.1.5-0+deb13u1 |
| libswscale-dev | 7:7.1.5-0+deb13u1 |
| libswresample-dev | 7:7.1.5-0+deb13u1 |
| libsdl3-dev | 3.2.10+ds-1 |
| pkg-config | 1.8.1-4 |

### Pinned versions (pkg-config, as resolved by Meson)

| Module | Resolved version | Floor in meson.build |
|---|---|---|
| gtk4 | 4.18.6 | >= 4.10 |
| glib-2.0 | 2.84.4 | >= 2.74 |
| gobject-2.0 | 2.84.4 | >= 2.74 |
| json-glib-1.0 | 1.10.6 | >= 1.6 |
| libavformat | 61.7.103 | >= 60 |
| libavcodec | 61.19.101 | >= 60 |
| libavutil | 59.39.100 | >= 58 |
| libavfilter | 10.5.100 | >= 9 |
| libswscale | 8.3.100 | >= 7 |
| libswresample | 5.3.100 | >= 4 |
| sdl3 | 3.2.10 | >= 3.2 |

## Build, test, run

All commands run from this directory (`obvious-edit/`):

    meson setup build && ninja -C build     # zero warnings, werror
    meson test -C build                     # 18 test suites
    ./scripts/run-headless.sh               # headless self-check, exit 0

The headless script opens the Obvious Edit window under Xvfb, quits after
the first map, and exits 0. To prove the window title on a live run:

    xwininfo -root -tree   # while a run is in flight: "Obvious Edit"

### Verification gates

    meson setup build-san -Db_sanitize=address,undefined
    meson test -C build-san
    meson test -C build --wrapper "valgrind --tool=memcheck --leak-check=full --error-exitcode=1 --suppressions=$(pwd)/tests/valgrind.supp"
    clang-format --dry-run --Werror src/*.c src/*/*.c src/*/*.h tests/*.c

`tests/valgrind.supp` suppresses only GLib/GObject one-time internal
allocations; the lifecycle test runs with `SDL_AUDIODRIVER=dummy` so it is
hermetic (no host audio device, no third-party audio-library noise).

## Documentation

- `docs/architecture.md` — how the pieces fit
- `docs/project-format.md` — the (future) project file format
- `docs/code-map.md` — what lives in which file
- `docs/glossary.md` — NLE vocabulary used in this codebase
- `docs/learning/phase-0.md` — the Phase 0 walkthrough for new contributors
- `docs/learning/phase-1.md` — the Phase 1 walkthrough (shell, commands, layout)
- `docs/learning/phase-2.md` — the Phase 2 walkthrough (media import, bin, inspector)

## License

MIT — see `LICENSE`.
