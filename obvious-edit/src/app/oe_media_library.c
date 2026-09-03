/* oe_media_library.c — session asset records with monitoring (Phase 2).
 *
 * Single-threaded by contract: constructed, mutated, and freed on the
 * main thread. Monitors and observers therefore need no locking.
 */

#include "oe_media_library.h"

#include <string.h>

#include <gio/gio.h>

#include "oe_log.h"

typedef struct
{
  guint id;
  OeMediaLibrary *library; /* back pointer for monitor callbacks */
  gchar *path;
  gchar *name;
  OeAssetStatus status;
  OeProbeInfo info;
  OeThumbnail thumbnail;
  GFileMonitor *monitor;
  gulong monitor_handler;
} Asset;

struct _OeMediaLibrary
{
  GPtrArray *assets; /* Asset*, insertion order */
  guint next_id;
  OeLibraryChangedFunc observer;
  gpointer observer_data;
};

static void
asset_free (Asset *asset)
{
  if (asset->monitor != NULL)
    {
      if (asset->monitor_handler != 0)
        g_signal_handler_disconnect (asset->monitor, asset->monitor_handler);
      g_object_unref (asset->monitor);
    }
  oe_probe_info_clear (&asset->info);
  g_clear_pointer (&asset->thumbnail.rgba, g_free);
  g_free (asset->name);
  g_free (asset->path);
  g_free (asset);
}

static Asset *
find_asset (OeMediaLibrary *library, guint id)
{
  for (guint i = 0; i < library->assets->len; i++)
    {
      Asset *asset = g_ptr_array_index (library->assets, i);

      if (asset->id == id)
        return asset;
    }
  return NULL;
}

static void
notify (OeMediaLibrary *library, guint id)
{
  if (library->observer != NULL)
    library->observer (id, library->observer_data);
}

/* Monitors fire on the main context; a deletion moves the asset to
 * MISSING and stops watching (nothing left to watch). */
static void
on_monitor_changed (GFileMonitor *monitor G_GNUC_UNUSED, GFile *file G_GNUC_UNUSED,
                    GFile *other_file G_GNUC_UNUSED, GFileMonitorEvent event_type,
                    gpointer user_data)
{
  Asset *asset = user_data;

  if (event_type != G_FILE_MONITOR_EVENT_DELETED && event_type != G_FILE_MONITOR_EVENT_MOVED_OUT)
    return;

  if (asset->status != OE_ASSET_STATUS_OK)
    return;

  oe_log (OE_LOG_LEVEL_DEBUG, "asset '%s' disappeared: marking missing", asset->path);
  oe_media_library_mark_missing (asset->library, asset->id);
}

OeMediaLibrary *
oe_media_library_new (void)
{
  OeMediaLibrary *library = g_new0 (OeMediaLibrary, 1);

  library->assets = g_ptr_array_new_with_free_func ((GDestroyNotify) asset_free);
  library->next_id = 1;
  return library;
}

void
oe_media_library_free (OeMediaLibrary *library)
{
  if (library == NULL)
    return;

  g_ptr_array_unref (library->assets);
  g_free (library);
}

void
oe_media_library_set_observer (OeMediaLibrary *library, OeLibraryChangedFunc observer,
                               gpointer user_data)
{
  g_return_if_fail (library != NULL);

  library->observer = observer;
  library->observer_data = user_data;
}

const gchar *
oe_asset_status_get_name (OeAssetStatus status)
{
  switch (status)
    {
    case OE_ASSET_STATUS_IMPORTING:
      return "Importing";
    case OE_ASSET_STATUS_OK:
      return "OK";
    case OE_ASSET_STATUS_MISSING:
      return "Missing";
    case OE_ASSET_STATUS_UNSUPPORTED:
      return "Unsupported";
    default:
      return "Unknown";
    }
}

void
oe_asset_info_init (OeAssetInfo *info)
{
  memset (info, 0, sizeof (*info));
}

void
oe_asset_info_clear (OeAssetInfo *info)
{
  g_clear_pointer (&info->path, g_free);
  g_clear_pointer (&info->name, g_free);
  oe_probe_info_clear (&info->info);
  g_clear_pointer (&info->thumbnail.rgba, g_free);
  memset (info, 0, sizeof (*info));
}

void
oe_asset_info_copy (OeAssetInfo *dst, const OeAssetInfo *src)
{
  oe_asset_info_clear (dst);
  dst->id = src->id;
  dst->path = g_strdup (src->path);
  dst->name = g_strdup (src->name);
  dst->status = src->status;
  oe_probe_info_copy (&dst->info, &src->info);

  if (src->thumbnail.rgba != NULL)
    {
      dst->thumbnail.width = src->thumbnail.width;
      dst->thumbnail.height = src->thumbnail.height;
      dst->thumbnail.rgba = g_memdup2 (src->thumbnail.rgba,
                                       (gsize) src->thumbnail.width * src->thumbnail.height * 4);
    }
}

guint
oe_media_library_add (OeMediaLibrary *library, const gchar *path)
{
  g_return_val_if_fail (library != NULL, 0);
  g_return_val_if_fail (path != NULL && path[0] != '\0', 0);

  Asset *asset = g_new0 (Asset, 1);

  asset->id = library->next_id++;
  asset->library = library;
  asset->path = g_strdup (path);
  asset->name = g_path_get_basename (path);
  asset->status = OE_ASSET_STATUS_IMPORTING;
  oe_probe_info_init (&asset->info);

  g_ptr_array_add (library->assets, asset);
  notify (library, asset->id);

  oe_log (OE_LOG_LEVEL_DEBUG, "asset %u added: '%s'", asset->id, path);
  return asset->id;
}

/* Starts (or restarts) the deletion monitor for an OK asset. */
static void
start_monitor (Asset *asset)
{
  GFile *file = g_file_new_for_path (asset->path);
  GError *error = NULL;
  GFileMonitor *monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, &error);

  if (monitor == NULL)
    {
      /* No monitor means external deletions go unnoticed until the file
       * is touched again — log it, never fail the import for it. */
      oe_log (OE_LOG_LEVEL_WARNING, "no file monitor for '%s': %s", asset->path,
              error != NULL ? error->message : "unknown reason");
      g_clear_error (&error);
      g_object_unref (file);
      return;
    }

  asset->monitor = monitor;
  asset->monitor_handler
      = g_signal_connect (monitor, "changed", G_CALLBACK (on_monitor_changed), asset);
  g_object_unref (file);
}

static void
stop_monitor (Asset *asset)
{
  if (asset->monitor == NULL)
    return;

  if (asset->monitor_handler != 0)
    {
      g_signal_handler_disconnect (asset->monitor, asset->monitor_handler);
      asset->monitor_handler = 0;
    }
  g_clear_object (&asset->monitor);
}

gboolean
oe_media_library_mark_ok (OeMediaLibrary *library, guint id, const OeProbeInfo *info)
{
  g_return_val_if_fail (library != NULL, FALSE);
  g_return_val_if_fail (info != NULL, FALSE);

  Asset *asset = find_asset (library, id);

  if (asset == NULL || asset->status != OE_ASSET_STATUS_IMPORTING)
    return FALSE;

  stop_monitor (asset);
  oe_probe_info_clear (&asset->info);
  oe_probe_info_copy (&asset->info, info);
  asset->status = OE_ASSET_STATUS_OK;
  start_monitor (asset);
  notify (library, id);

  oe_log (OE_LOG_LEVEL_DEBUG, "asset %u OK: '%s'", id, asset->path);
  return TRUE;
}

gboolean
oe_media_library_mark_unsupported (OeMediaLibrary *library, guint id)
{
  g_return_val_if_fail (library != NULL, FALSE);

  Asset *asset = find_asset (library, id);

  if (asset == NULL || asset->status != OE_ASSET_STATUS_IMPORTING)
    return FALSE;

  stop_monitor (asset);
  asset->status = OE_ASSET_STATUS_UNSUPPORTED;
  notify (library, id);
  return TRUE;
}

gboolean
oe_media_library_mark_missing (OeMediaLibrary *library, guint id)
{
  g_return_val_if_fail (library != NULL, FALSE);

  Asset *asset = find_asset (library, id);

  /* OK → MISSING comes from the monitor; IMPORTING → MISSING from a
   * failed import. MISSING → MISSING is not a transition. */
  if (asset == NULL || asset->status == OE_ASSET_STATUS_MISSING)
    return FALSE;

  stop_monitor (asset);
  asset->status = OE_ASSET_STATUS_MISSING;
  notify (library, id);
  return TRUE;
}

void
oe_media_library_remove (OeMediaLibrary *library, guint id)
{
  g_return_if_fail (library != NULL);

  for (guint i = 0; i < library->assets->len; i++)
    {
      Asset *asset = g_ptr_array_index (library->assets, i);

      if (asset->id != id)
        continue;

      g_ptr_array_remove_index (library->assets, i);
      notify (library, id); /* observers see the removal (get() now fails) */
      return;
    }
}

gboolean
oe_media_library_relink (OeMediaLibrary *library, guint id, const gchar *path)
{
  g_return_val_if_fail (library != NULL, FALSE);
  g_return_val_if_fail (path != NULL && path[0] != '\0', FALSE);

  Asset *asset = find_asset (library, id);

  if (asset == NULL)
    return FALSE;

  stop_monitor (asset);
  g_free (asset->path);
  asset->path = g_strdup (path);
  g_free (asset->name);
  asset->name = g_path_get_basename (path);
  oe_probe_info_clear (&asset->info);
  g_clear_pointer (&asset->thumbnail.rgba, g_free);
  asset->thumbnail.width = 0;
  asset->thumbnail.height = 0;
  asset->status = OE_ASSET_STATUS_IMPORTING;
  notify (library, id);

  oe_log (OE_LOG_LEVEL_DEBUG, "asset %u relinked to '%s'", id, path);
  return TRUE;
}

gboolean
oe_media_library_get (OeMediaLibrary *library, guint id, OeAssetInfo *out)
{
  g_return_val_if_fail (library != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);

  Asset *asset = find_asset (library, id);

  if (asset == NULL)
    return FALSE;

  OeAssetInfo info;

  oe_asset_info_init (&info);
  info.id = asset->id;
  info.path = g_strdup (asset->path);
  info.name = g_strdup (asset->name);
  info.status = asset->status;
  oe_probe_info_copy (&info.info, &asset->info);

  if (asset->thumbnail.rgba != NULL)
    {
      info.thumbnail.width = asset->thumbnail.width;
      info.thumbnail.height = asset->thumbnail.height;
      info.thumbnail.rgba = g_memdup2 (asset->thumbnail.rgba, (gsize) asset->thumbnail.width
                                                                  * asset->thumbnail.height * 4);
    }

  oe_asset_info_clear (out);
  *out = info;
  return TRUE;
}

void
oe_media_library_set_thumbnail (OeMediaLibrary *library, guint id, const OeThumbnail *thumb)
{
  g_return_if_fail (library != NULL);

  Asset *asset = find_asset (library, id);

  if (asset == NULL)
    return;

  g_clear_pointer (&asset->thumbnail.rgba, g_free);
  asset->thumbnail.width = 0;
  asset->thumbnail.height = 0;

  if (thumb != NULL && thumb->rgba != NULL && thumb->width > 0 && thumb->height > 0)
    {
      asset->thumbnail.width = thumb->width;
      asset->thumbnail.height = thumb->height;
      asset->thumbnail.rgba = g_memdup2 (thumb->rgba, (gsize) thumb->width * thumb->height * 4);
    }
}

guint
oe_media_library_count (OeMediaLibrary *library)
{
  g_return_val_if_fail (library != NULL, 0);

  return library->assets->len;
}

GList *
oe_media_library_list_ids (OeMediaLibrary *library)
{
  g_return_val_if_fail (library != NULL, NULL);

  GList *ids = NULL;

  for (guint i = library->assets->len; i > 0; i--)
    {
      Asset *asset = g_ptr_array_index (library->assets, i - 1);

      ids = g_list_prepend (ids, GUINT_TO_POINTER (asset->id));
    }
  return ids;
}
