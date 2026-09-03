/* oe_shell_layout.h — shell layout persistence (Phase 1).
 *
 * The window size and panel splitter positions are application state, not
 * project data: they live in a GKeyFile at
 * $XDG_CONFIG_HOME/obvious-edit/layout.conf with a version group starting
 * at 1. The save/load logic is GTK-free on purpose — it operates on the
 * plain OeShellLayout struct so the round-trip is unit-testable without a
 * display; the widget layer only reads and writes that struct.
 *
 * Load rules (never fail the launch over layout):
 *   - missing file  -> documented defaults,
 *   - corrupt file  -> defaults + warning log,
 *   - newer version -> defaults + warning log,
 *   - corrupt fields-> clamped to sane minimums.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/** The layout schema version written by this build. */
#define OE_SHELL_LAYOUT_VERSION 1

/** Sane minimums: values below these clamp on load. */
#define OE_SHELL_LAYOUT_MIN_WIDTH 640
#define OE_SHELL_LAYOUT_MIN_HEIGHT 420
#define OE_SHELL_LAYOUT_MIN_BIN_WIDTH 160
#define OE_SHELL_LAYOUT_MIN_INSPECTOR_WIDTH 160
#define OE_SHELL_LAYOUT_MIN_TIMELINE_HEIGHT 120

/**
 * OeShellLayout:
 * @version: layout schema version, starts at #OE_SHELL_LAYOUT_VERSION.
 * @window_width, @window_height: window size in px.
 * @window_maximized: whether the window was maximized at close.
 * @bin_width: media-bin GtkPaned divider position, in px.
 * @inspector_width: inspector GtkPaned divider position, in px.
 * @timeline_height: timeline GtkPaned divider position, in px.
 */
typedef struct
{
  int version;
  int window_width;
  int window_height;
  gboolean window_maximized;
  int bin_width;
  int inspector_width;
  int timeline_height;
} OeShellLayout;

/**
 * oe_shell_layout_defaults:
 * @l: layout to fill
 *
 * Fills @l with the documented defaults: 1280x720, not maximized,
 * bin 280 / inspector 320 / timeline 380.
 */
void oe_shell_layout_defaults (OeShellLayout *l);

/**
 * oe_shell_layout_default_path:
 *
 * Returns: (transfer full): the layout file path,
 * $XDG_CONFIG_HOME/obvious-edit/layout.conf (XDG_CONFIG_HOME honoured).
 * Free with g_free().
 */
gchar *oe_shell_layout_default_path (void);

/**
 * oe_shell_layout_save:
 * @l: layout to write
 * @error: return location for a GError
 *
 * Saves to the default path. The write is atomic (temp file + rename), so
 * a crash mid-write cannot corrupt the previous file.
 *
 * Returns: TRUE on success; FALSE with @error set when the file could not
 * be written. Callers log and continue — layout loss is not fatal.
 */
gboolean oe_shell_layout_save (const OeShellLayout *l, GError **error);

/**
 * oe_shell_layout_load:
 * @l: layout to fill
 * @error: unused; provided for signature symmetry
 *
 * Loads from the default path following the load rules above. Never fails
 * the launch: any problem falls back to defaults (with a log line) and
 * returns TRUE. FALSE means a programming error (NULL argument).
 *
 * Returns: TRUE with a usable @l in all normal cases.
 */
gboolean oe_shell_layout_load (OeShellLayout *l, GError **error);

/**
 * oe_shell_layout_save_to:
 * @l: layout to write
 * @path: explicit file path (tests and tools use this)
 * @error: return location for a GError
 *
 * Returns: TRUE on success; FALSE with @error set otherwise.
 */
gboolean oe_shell_layout_save_to (const OeShellLayout *l, const gchar *path, GError **error);

/**
 * oe_shell_layout_load_from:
 * @l: layout to fill
 * @path: explicit file path (tests and tools use this)
 * @error: unused; provided for signature symmetry
 *
 * Returns: TRUE with a usable @l in all normal cases (see load rules).
 */
gboolean oe_shell_layout_load_from (OeShellLayout *l, const gchar *path, GError **error);

G_END_DECLS
