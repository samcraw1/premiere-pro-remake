/* oe_media_bin.h — the media bin panel (Phase 2).
 *
 * The bin is the Phase 2 replacement for the placeholder panel: labeled
 * frame, empty state, and one row per session asset (thumbnail, name,
 * kind+duration line, status badge). Rows carrying MISSING or
 * UNSUPPORTED status grow a Relink button; selection drives the
 * inspector. Dropping a file list onto the panel routes paths to the
 * same import entry point as the File ▸ Import Media chooser.
 *
 * GTK layer: textures are GdkMemoryTextures built from the library's
 * raw RGBA bytes; no decode happens here.
 */

#pragma once

#include <gtk/gtk.h>

#include "../app/oe_media_library.h"
#include "../media/oe_probe.h"

G_BEGIN_DECLS

#define OE_TYPE_MEDIA_BIN (oe_media_bin_get_type ())
G_DECLARE_FINAL_TYPE (OeMediaBin, oe_media_bin, OE, MEDIA_BIN, GtkWidget)

/**
 * oe_media_bin_new:
 * @library: the session asset store driving the rows.
 *
 * The bin does not own @library; the window keeps both alive.
 */
OeMediaBin *oe_media_bin_new (OeMediaLibrary *library);

/**
 * oe_media_bin_set_import_func:
 * @func: called with a NULL-terminated array of paths, then @user_data.
 *
 * Drop-target sink: dropped files land on the same import entry point
 * the chooser uses. Clear with NULL before the owner dies.
 */
typedef void (*OeMediaBinImportFunc) (const gchar *const *paths, gpointer user_data);

void oe_media_bin_set_import_func (OeMediaBin *bin, OeMediaBinImportFunc func, gpointer user_data);

/** Rebuilds rows from the library (call after any library mutation). */
void oe_media_bin_refresh (OeMediaBin *bin);

/** Selected asset id, or 0 when nothing is selected. */
guint oe_media_bin_get_selected (OeMediaBin *bin);

/**
 * Presentation helpers shared with the inspector (pure functions).
 */
const gchar *oe_media_bin_kind_name (OeMediaKind kind);
gchar *oe_media_bin_format_duration_us (gint64 duration_us);

G_END_DECLS
