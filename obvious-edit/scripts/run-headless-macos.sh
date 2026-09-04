#!/bin/sh
# Runs the already-built binary's headless self-check natively on macOS and
# exits 0 on success.
#
# macOS has no analog of Xvfb (there is no virtual framebuffer X server) and
# no analog of dbus-run-session (GtkApplication's D-Bus session registration
# is a Linux-only feature that is simply skipped off Linux). Instead, macOS
# always has a real WindowServer available to any GUI session, so the
# binary is launched directly against GTK4's native quartz GDK backend --
# no forced GDK_BACKEND or GSK_RENDERER override is needed. --self-check
# unwinds the app on the window's first map, exactly as on Linux.
#
# Requires a GUI session that owns the WindowServer (an interactive Terminal
# or iTerm session, or a screen-sharing-attached CI runner). A bare SSH
# session into a Mac with nobody logged into the console does not have one;
# there is no macOS equivalent of Xvfb to fall back to in that case.
#
# The build directory defaults to ./build; override with BUILD_DIR=<path>.
set -eu

BUILD_DIR="${BUILD_DIR:-build}"
BIN="$BUILD_DIR/obvious-edit"

if [ ! -x "$BIN" ]; then
  echo "run-headless-macos.sh: $BIN not found or not executable." >&2
  echo "Build first: meson setup build && ninja -C build" >&2
  exit 1
fi

exec "$BIN" --self-check
