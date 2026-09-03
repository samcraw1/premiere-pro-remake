/* oe_media_library.h — session asset records with monitoring (Phase 2).
 *
 * GTK-free by design: records are plain structs, observers are plain
 * function pointers, and every mutation is made on the main thread
 * (GFileMonitor emits there by default). Asset ids are opaque guint
 * counters; there is no persistence — records are session-transient
 * (project-format.md).
 *
 * Status model:
 *   IMPORTING    added, awaiting the import worker's verdict
 *   OK           probed successfully; a file monitor watches the path
 *   MISSING      monitored file disappeared or its probe said
 *                OPEN_FAILED; stays in the bin for relinking
 *   UNSUPPORTED  probe rejected the content; stays in the bin only
 *                when it was a relink attempt (fresh imports of
 *                unsupported files are never added — the caller
 *                removes the IMPORTING record and reports a message)
 */

#pragma once

#include <glib.h>

#include "../media/oe_media_jobs.h"
#include "../media/oe_probe.h"

G_BEGIN_DECLS

/**
 * OeAssetStatus:
 * @OE_ASSET_STATUS_IMPORTING: import still running.
 * @OE_ASSET_STATUS_OK: probed; metadata and monitor live.
 * @OE_ASSET_STATUS_MISSING: file vanished (or unopenable at import).
 * @OE_ASSET_STATUS_UNSUPPORTED: content has no decodable A/V stream.
 */
typedef enum
{
  OE_ASSET_STATUS_IMPORTING,
  OE_ASSET_STATUS_OK,
  OE_ASSET_STATUS_MISSING,
  OE_ASSET_STATUS_UNSUPPORTED,
} OeAssetStatus;

const gchar *oe_asset_status_get_name (OeAssetStatus status);

/**
 * OeAssetInfo: value copy of one library record.
 * @id: opaque asset id (never 0).
 * @path: current file path (owned by the copy after oe_asset_info_copy).
 * @name: display name, the path basename.
 * @status: current status.
 * @info: probed metadata; meaningful only when @status is OK.
 * @thumbnail: raw RGBA preview (owned); zeroed until the import worker
 *     delivers one.
 */
typedef struct
{
  guint id;
  gchar *path;
  gchar *name;
  OeAssetStatus status;
  OeProbeInfo info;
  OeThumbnail thumbnail;
} OeAssetInfo;

void oe_asset_info_init (OeAssetInfo *info);
void oe_asset_info_clear (OeAssetInfo *info);
void oe_asset_info_copy (OeAssetInfo *dst, const OeAssetInfo *src);

/**
 * OeLibraryChangedFunc: observer for any bin-affecting change.
 * @asset_id: id of the record that changed (or was removed).
 * @user_data: context pointer supplied at connect time.
 */
typedef void (*OeLibraryChangedFunc) (guint asset_id, gpointer user_data);

typedef struct _OeMediaLibrary OeMediaLibrary;

OeMediaLibrary *oe_media_library_new (void);
void oe_media_library_free (OeMediaLibrary *library);

void oe_media_library_set_observer (OeMediaLibrary *library, OeLibraryChangedFunc observer,
                                    gpointer user_data);

/**
 * oe_media_library_add:
 * @path: file path to track (copied).
 *
 * Adds a record in IMPORTING state and notifies the observer.
 * Returns: the new opaque asset id.
 */
guint oe_media_library_add (OeMediaLibrary *library, const gchar *path);

/**
 * oe_media_library_set_thumbnail:
 * @thumb: raw RGBA preview to copy, or NULL to clear.
 *
 * Stores the decoded preview bytes on the record (GTK-free raw data;
 * the UI turns them into a GdkMemoryTexture). Unknown ids are ignored.
 */
void oe_media_library_set_thumbnail (OeMediaLibrary *library, guint id, const OeThumbnail *thumb);

/**
 * oe_media_library_mark_ok:
 * @info: probed metadata (copied into the record).
 *
 * Transitions the record to OK and installs a GFileMonitor on its path.
 * Returns: TRUE if @id was a pending record.
 */
gboolean oe_media_library_mark_ok (OeMediaLibrary *library, guint id, const OeProbeInfo *info);

/**
 * oe_media_library_mark_unsupported:
 *
 * Marks a relinked record UNSUPPORTED (the row stays for another
 * relink). Returns: TRUE if @id was a pending record.
 */
gboolean oe_media_library_mark_unsupported (OeMediaLibrary *library, guint id);

/**
 * oe_media_library_mark_missing:
 *
 * Marks a record MISSING, cancelling any monitor (import failure and
 * the monitor path share this transition).
 */
gboolean oe_media_library_mark_missing (OeMediaLibrary *library, guint id);

/**
 * oe_media_library_remove:
 *
 * Drops the record (fresh import of an unsupported file: "never added")
 * and notifies the observer. Unknown ids are ignored.
 */
void oe_media_library_remove (OeMediaLibrary *library, guint id);

/**
 * oe_media_library_relink:
 * @path: replacement path (copied).
 *
 * Re-points an OK/MISSING/UNSUPPORTED record at @path, cancels any
 * monitor, and returns the record to IMPORTING for a re-probe.
 * Returns: TRUE on a known id; FALSE (no error) otherwise.
 */
gboolean oe_media_library_relink (OeMediaLibrary *library, guint id, const gchar *path);

/**
 * oe_media_library_get:
 * @out: receives a freshly initialized copy of the record; the caller
 * owns it and must clear it with oe_asset_info_clear when done.
 *
 * Returns: TRUE if @id is known.
 */
gboolean oe_media_library_get (OeMediaLibrary *library, guint id, OeAssetInfo *out);

guint oe_media_library_count (OeMediaLibrary *library);

/**
 * oe_media_library_list_ids:
 *
 * Returns: (transfer container): GList of guint ids (const data),
 * in insertion order. Free with g_list_free().
 */
GList *oe_media_library_list_ids (OeMediaLibrary *library);

G_END_DECLS
