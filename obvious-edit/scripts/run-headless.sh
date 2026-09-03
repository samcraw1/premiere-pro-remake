#!/bin/sh
# Builds nothing; runs the already-built binary headless and exits 0 on success.
#
# The recipe (dbus-run-session + xvfb-run, GDK_BACKEND=x11, GSK_RENDERER=cairo)
# is pinned because it was verified on Debian 13 without a GPU: the window
# needs a session bus for GtkApplication and an X server for GtkWindow, and
# the cairo renderer avoids GPU-dependent Vulkan/GL paths under Xvfb.
#
# The build directory defaults to ./build; override with BUILD_DIR=<path>.
set -eu

BUILD_DIR="${BUILD_DIR:-build}"
BIN="$BUILD_DIR/obvious-edit"

if [ ! -x "$BIN" ]; then
  echo "run-headless.sh: $BIN not found or not executable." >&2
  echo "Build first: meson setup build && ninja -C build" >&2
  exit 1
fi

exec dbus-run-session -- xvfb-run -a -s "-screen 0 1280x800x24" \
  env GDK_BACKEND=x11 GSK_RENDERER=cairo \
  "$BIN" --self-check
