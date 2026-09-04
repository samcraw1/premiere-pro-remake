/* oe_main_window.c — the editor shell (Phases 1 + 2).
 *
 * Composition rules for this file:
 *   - Layout is built from GtkPaned/GtkBox only. No GtkFixed, no absolute
 *     pixel placement: every panel is a splitter child and resizes with
 *     the window.
 *   - Every panel is labeled and shows an empty-state line that names the
 *     phase which will fill it.
 *   - The window is the seam between GTK and the GTK-free layers: it
 *     installs the reporter (status bar feedback) and the media.import
 *     handler (via the same static-seam pattern, since dispatch passes no
 *     user_data), and never calls registry internals.
 *   - Phase 2: the window owns the GTK-free media library and import
 *     worker, routes chooser and drag-and-drop paths through one import
 *     entry point, and turns worker verdicts into library transitions,
 *     status-bar messages, and inspector content.
 *   - Layout persistence flows through the OeShellLayout struct only:
 *     load at construction (defaults on first run), save on close-request.
 *   - Phase 3: the window drives File > New/Open/Save through the
 *     GTK-free project model and format modules. All parsing,
 *     validation, serialization, and writing lives in src/core; the
 *     window supplies choosers and status-bar feedback only. Every
 *     session is stamped with an epoch so import results from a
 *     replaced session are dropped instead of aliasing new rows.
 */

#include "oe_main_window.h"

#include <gtk/gtk.h>

#include "../app/oe_command.h"
#include "../app/oe_import_worker.h"
#include "../app/oe_log.h"
#include "../app/oe_media_library.h"
#include "../core/oe_project.h"
#include "../core/oe_project_format.h"
#include "oe_media_bin.h"
#include "oe_shell_layout.h"
#include "oe_timeline.h"

/* Pending splitter positions from the loaded layout, applied on first map
 * (GtkPaned positions are only meaningful once the window is allocated). */
struct _OeMainWindow
{
  GtkApplicationWindow parent_instance;

  GtkWidget *status_bar;
  GtkWidget *status_label;
  GtkWidget *bin_paned;
  GtkWidget *inspector_paned;
  GtkWidget *timeline_paned;

  /* Phase 2: owned GTK-free services and the panels that show them. */
  OeMediaLibrary *media_library;
  OeImportWorker *import_worker;
  GtkWidget *media_bin;

  /* Phase 3: the GTK-free project model, its on-disk anchor, and the
   * session epoch stamped onto worker jobs (results carrying an older
   * epoch belong to a replaced session and are dropped). */
  OeProject *project;
  gchar *project_path;
  guint session_epoch;

  /* Phase 4: the live timeline view plus the session map between
   * project media references (stable, serialized) and bin asset ids
   * (session-transient). The widget resolves missing/kind/audio info
   * through the library via this map — it never probes during draws. */
  GtkWidget *timeline;
  GHashTable *media_ref_to_asset; /* guint ref → guint asset id */
  GHashTable *asset_to_media_ref; /* guint asset id → guint ref */

  /* TRUE while the current import batch belongs to a project open: the
   * generic "Imported N file(s)" summary would bury the Loaded
   * message, so it is suppressed until the batch drains. */
  gboolean import_batch_open;

  GtkWidget *inspector_stack; /* "empty" | "media" */
  GtkWidget *inspector_media; /* grid rebuilt per selection */

  guint import_pending; /* outstanding worker jobs in the current batch */
  guint import_ok;      /* OK verdicts since the batch started */
  guint relink_target;  /* asset id awaiting a relink chooser, else 0 */

  int pending_bin;
  int pending_inspector;
  int pending_timeline;
  gboolean positions_applied;
  gboolean layout_saved;
};

G_DEFINE_TYPE (OeMainWindow, oe_main_window, GTK_TYPE_APPLICATION_WINDOW)

/* ------------------------------------------------------------------ */
/* Static handler seam: command dispatch carries no user_data, so the  */
/* handler context rides a static pointer, installed at construction   */
/* and cleared in dispose — the same pattern as the reporter seam.     */
/* ------------------------------------------------------------------ */

static OeMainWindow *command_owner = NULL;

/* ------------------------------------------------------------------ */
/* Status reporting: every user-visible phase-2 message lands through   */
/* the same status bar the command reporter writes to.                  */
/* ------------------------------------------------------------------ */

/* Forward decls: the import callback repopulates the inspector before
 * the inspector section below defines it; the session reset re-attaches
 * the library observer defined further down. */
static void populate_inspector (OeMainWindow *self);
static void on_library_changed (guint asset_id, gpointer user_data);

static void
set_status_message (OeMainWindow *self, const gchar *message)
{
  gtk_label_set_text (GTK_LABEL (self->status_label), message);
}

static void
report_to_status_bar (OeCommandId id G_GNUC_UNUSED, const gchar *message, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);

  set_status_message (self, message);
}

/* ------------------------------------------------------------------ */
/* Import pipeline: one entry point for the chooser and drag-and-drop. */
/* ------------------------------------------------------------------ */

static void
import_paths (OeMainWindow *self, const gchar *const *paths)
{
  self->import_batch_open = FALSE;

  for (guint i = 0; paths != NULL && paths[i] != NULL; i++)
    {
      guint id = oe_media_library_add (self->media_library, paths[i]);

      /* The observer refreshed the bin (IMPORTING row); the worker's
       * verdict will move the record to OK, MISSING, or UNSUPPORTED. */
      oe_import_worker_submit (self->import_worker, paths[i], id, FALSE,
                               GUINT_TO_POINTER (self->session_epoch));
      self->import_pending++;
    }
}

/* Phase 4 session map helpers (defined with the timeline seams below);
 * the import verdict and project-open flows run earlier in the file. */
static void register_media_asset_pair (OeMainWindow *self, guint media_ref, guint asset_id);
static guint lookup_media_ref_for_asset (OeMainWindow *self, guint asset_id);

static void
on_import_done (const OeImportJobResult *result, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);

  /* A result stamped with an older epoch belongs to a replaced session:
   * asset ids are a per-library sequence, so applying it to the current
   * library could alias a new row. Drop it untouched. */
  if (GPOINTER_TO_UINT (result->tag) != self->session_epoch)
    return;

  switch (result->result)
    {
    case OE_IMPORT_RESULT_OK:
      oe_media_library_mark_ok (self->media_library, result->asset_id, &result->info);
      oe_media_library_set_thumbnail (self->media_library, result->asset_id, &result->thumbnail);

      /* Phase 4: annotate the project's media record with the probed
       * source duration so trim validation has AV bounds. Stills probe
       * 0 and stay unannotated (unbounded, uniform-duration rule).
       * Session-only: never serialized. */
      {
        guint media_ref = lookup_media_ref_for_asset (self, result->asset_id);

        if (media_ref != 0 && result->info.duration_us > 0)
          oe_project_set_media_source_duration (self->project, media_ref, result->info.duration_us);
      }

      self->import_ok++;
      break;

    case OE_IMPORT_RESULT_MISSING:
      /* The file vanished between add and probe: keep the row and
       * offer relinking, like any other missing asset. */
      oe_media_library_mark_missing (self->media_library, result->asset_id);
      {
        OeAssetInfo info;

        oe_asset_info_init (&info);
        if (oe_media_library_get (self->media_library, result->asset_id, &info))
          {
            g_autofree gchar *msg = g_strdup_printf ("Missing: '%s'", info.name);

            set_status_message (self, msg);
            oe_asset_info_clear (&info);
          }
      }
      break;

    case OE_IMPORT_RESULT_UNSUPPORTED:
      if (result->relink)
        {
          /* A relink attempt may legitimately fail: the row stays for
           * another try. Fresh imports never show unsupported files. */
          oe_media_library_mark_unsupported (self->media_library, result->asset_id);
        }
      else
        {
          OeAssetInfo info;

          oe_asset_info_init (&info);
          if (oe_media_library_get (self->media_library, result->asset_id, &info))
            {
              g_autofree gchar *msg = g_strdup_printf ("Unsupported: '%s'", info.name);

              set_status_message (self, msg);
              oe_asset_info_clear (&info);
            }
          oe_media_library_remove (self->media_library, result->asset_id);
        }
      break;

    case OE_IMPORT_RESULT_CANCELLED:
      /* Only reachable during shutdown drain: drop the in-flight row. */
      oe_media_library_remove (self->media_library, result->asset_id);
      break;

    default:
      break;
    }

  if (self->import_pending > 0)
    self->import_pending--;

  if (self->import_pending == 0 && self->import_ok > 0 && !self->import_batch_open)
    {
      g_autofree gchar *msg = g_strdup_printf ("Imported %u file(s)", self->import_ok);

      set_status_message (self, msg);
      self->import_ok = 0;
    }

  if (self->import_pending == 0)
    self->import_batch_open = FALSE;

  /* The selected row may have changed underneath the inspector. */
  guint selected = oe_media_bin_get_selected (OE_MEDIA_BIN (self->media_bin));

  if (selected == result->asset_id)
    populate_inspector (self);
}

/* ------------------------------------------------------------------ */
/* Inspector: full probed record for the selected asset.               */
/* ------------------------------------------------------------------ */

static GtkWidget *
inspector_key_label (const gchar *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 1.0);
  gtk_widget_add_css_class (label, "inspector-key");
  return label;
}

static GtkWidget *
inspector_value_label (const gchar *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, "inspector-value");
  return label;
}

static void
inspector_add_row (GtkWidget *grid, int *row, const gchar *key, const gchar *value)
{
  gtk_grid_attach (GTK_GRID (grid), inspector_key_label (key), 0, *row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), inspector_value_label (value), 1, *row, 1, 1);
  (*row)++;
}

static void
inspector_clear_grid (GtkWidget *grid)
{
  GtkWidget *child = gtk_widget_get_first_child (grid);

  while (child != NULL)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);

      gtk_grid_remove (GTK_GRID (grid), child);
      child = next;
    }
}

/* Shows the full probed record; blank lines are omitted. */
static void
show_media_info (OeMainWindow *self, const OeAssetInfo *info)
{
  GtkWidget *grid = self->inspector_media;

  inspector_clear_grid (grid);

  int row = 0;
  g_autofree gchar *id_text = g_strdup_printf ("%u", info->id);

  inspector_add_row (grid, &row, "Name", info->name);
  inspector_add_row (grid, &row, "Status", oe_asset_status_get_name (info->status));
  inspector_add_row (grid, &row, "Kind", oe_media_bin_kind_name (info->info.kind));

  if (info->info.duration_us > 0)
    {
      g_autofree gchar *duration = oe_media_bin_format_duration_us (info->info.duration_us);

      inspector_add_row (grid, &row, "Duration", duration);
    }

  if (info->info.width > 0 && info->info.height > 0)
    {
      g_autofree gchar *dims = g_strdup_printf ("%d × %d px", info->info.width, info->info.height);

      inspector_add_row (grid, &row, "Dimensions", dims);
    }

  if (info->info.frame_rate_num > 0 && info->info.frame_rate_den > 0)
    {
      g_autofree gchar *fps
          = g_strdup_printf ("%d / %d fps", info->info.frame_rate_num, info->info.frame_rate_den);

      inspector_add_row (grid, &row, "Frame rate", fps);
    }

  if (info->info.sample_rate > 0)
    {
      g_autofree gchar *rate = g_strdup_printf ("%d Hz", info->info.sample_rate);

      inspector_add_row (grid, &row, "Sample rate", rate);
    }

  if (info->info.channels > 0)
    {
      g_autofree gchar *channels = g_strdup_printf ("%d", info->info.channels);

      inspector_add_row (grid, &row, "Channels", channels);
    }

  if (info->info.container_name != NULL && info->info.container_name[0] != '\0')
    inspector_add_row (grid, &row, "Container", info->info.container_name);

  if (info->info.video_codec != NULL && info->info.video_codec[0] != '\0')
    inspector_add_row (grid, &row, "Video codec", info->info.video_codec);

  if (info->info.audio_codec != NULL && info->info.audio_codec[0] != '\0')
    inspector_add_row (grid, &row, "Audio codec", info->info.audio_codec);

  inspector_add_row (grid, &row, "Path", info->path);

  gtk_stack_set_visible_child_name (GTK_STACK (self->inspector_stack), "media");
}

/* Re-populates the inspector from the current bin selection (or clears
 * it back to the preserved empty state). */
static void
populate_inspector (OeMainWindow *self)
{
  guint id = oe_media_bin_get_selected (OE_MEDIA_BIN (self->media_bin));
  OeAssetInfo info;

  oe_asset_info_init (&info);

  if (id != 0 && oe_media_library_get (self->media_library, id, &info))
    {
      show_media_info (self, &info);
      oe_asset_info_clear (&info);
      return;
    }

  gtk_stack_set_visible_child_name (GTK_STACK (self->inspector_stack), "empty");
}

static void
on_bin_selection_changed (OeMediaBin *bin G_GNUC_UNUSED, gpointer user_data)
{
  populate_inspector (OE_MAIN_WINDOW (user_data));
}

/* ------------------------------------------------------------------ */
/* File chooser: media.import (multi-select) and relink (single).      */
/* The extension filter only narrows the picker; the probe remains the */
/* accept/reject authority.                                            */
/* ------------------------------------------------------------------ */

static void
add_media_filter (GtkFileDialog *dialog)
{
  GtkFileFilter *filter = gtk_file_filter_new ();

  gtk_file_filter_set_name (filter, "Media files");
  static const gchar *patterns[] = {
    "*.mp4",  "*.mov", "*.avi", "*.mkv", "*.webm", "*.m4v", "*.mpg",  "*.mpeg", "*.wav", "*.mp3",
    "*.flac", "*.ogg", "*.m4a", "*.aac", "*.png",  "*.jpg", "*.jpeg", "*.bmp",  "*.gif",
  };

  for (gsize i = 0; i < G_N_ELEMENTS (patterns); i++)
    gtk_file_filter_add_pattern (filter, patterns[i]);

  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  g_object_unref (filters);
  g_object_unref (filter);
}

static void on_relink_dialog_done (GObject *source, GAsyncResult *result, gpointer user_data);

static void
on_open_dialog_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);
  GError *error = NULL;

  GListModel *files
      = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (source), result, &error);

  if (files == NULL)
    {
      /* A canceled chooser is a no-op; real failures surface. */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          oe_log (OE_LOG_LEVEL_WARNING, "open dialog failed: %s", error->message);
          set_status_message (self, "Could not open the file chooser");
        }
      g_clear_error (&error);
      return;
    }

  GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < g_list_model_get_n_items (files); i++)
    {
      GFile *file = g_list_model_get_item (files, i);

      const gchar *path = g_file_peek_path (file);

      if (path != NULL)
        g_ptr_array_add (paths, g_strdup (path));
      g_object_unref (file);
    }
  g_object_unref (files);

  g_ptr_array_add (paths, NULL);
  import_paths (self, (const gchar *const *) paths->pdata);
  g_ptr_array_unref (paths);
}

static void
open_import_dialog (OeMainWindow *self)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();

  gtk_file_dialog_set_title (dialog, "Import Media");
  add_media_filter (dialog);

  gtk_file_dialog_open_multiple (dialog, GTK_WINDOW (self), NULL, on_open_dialog_done, self);
  g_object_unref (dialog);
}

static void
on_relink_dialog_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);
  guint target = self->relink_target;

  self->relink_target = 0;

  GError *error = NULL;

  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          oe_log (OE_LOG_LEVEL_WARNING, "relink dialog failed: %s", error->message);
          set_status_message (self, "Could not open the file chooser");
        }
      g_clear_error (&error);
      return;
    }

  const gchar *path = g_file_peek_path (file);

  if (path != NULL && target != 0 && oe_media_library_relink (self->media_library, target, path))
    {
      /* Back to IMPORTING; the re-probe runs through the same worker. */
      oe_import_worker_submit (self->import_worker, path, target, TRUE,
                               GUINT_TO_POINTER (self->session_epoch));
    }
  g_object_unref (file);
}

static void
on_relink_requested (OeMediaBin *bin G_GNUC_UNUSED, guint asset_id, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);

  self->relink_target = asset_id;

  GtkFileDialog *dialog = gtk_file_dialog_new ();

  gtk_file_dialog_set_title (dialog, "Relink Media");
  add_media_filter (dialog);

  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_relink_dialog_done, self);
  g_object_unref (dialog);
}

/* Drop sink wired by the bin: same entry point as the chooser. */
static void
bin_import_sink (const gchar *const *paths, gpointer user_data)
{
  import_paths (OE_MAIN_WINDOW (user_data), paths);
}

/* media.import handler: dispatch passes no user_data, so the context
 * comes from the static seam installed at construction. */
static void
media_import_command_handler (OeCommandId id G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (command_owner != NULL)
    open_import_dialog (command_owner);
}

/* ------------------------------------------------------------------ */
/* Project session: File > New/Open/Save. All parse/validate/apply/    */
/* serialize/write behavior lives in src/core (oe_project_format); the */
/* window only supplies choosers and status-bar feedback.              */
/* ------------------------------------------------------------------ */

/* Replaces the whole session: the model and the library. Asset ids are
 * a per-library sequence, so an in-flight result from the outgoing
 * session could alias a new row; every job submitted after this point
 * carries the bumped epoch, and on_import_done drops older tags. */
static void
reset_session (OeMainWindow *self, OeProject *project)
{
  g_clear_pointer (&self->project_path, g_free);
  g_clear_object (&self->project);
  g_clear_pointer (&self->media_library, oe_media_library_free);

  self->media_library = oe_media_library_new ();
  oe_media_library_set_observer (self->media_library, on_library_changed, self);
  oe_media_bin_set_library (OE_MEDIA_BIN (self->media_bin), self->media_library);
  oe_media_bin_refresh (OE_MEDIA_BIN (self->media_bin));

  /* Phase 4: the ref↔asset pairs belong to the replaced library, and
   * the timeline re-observes the replacement project (playhead and
   * selection reset inside the widget). */
  g_hash_table_remove_all (self->media_ref_to_asset);
  g_hash_table_remove_all (self->asset_to_media_ref);
  oe_timeline_set_project (OE_TIMELINE (self->timeline), project);

  self->project = project;
  self->session_epoch++;
  populate_inspector (self);
}

static void
save_project_to_path (OeMainWindow *self, const gchar *path)
{
  GError *error = NULL;

  if (!oe_project_format_save (self->project, path, &error))
    {
      g_autofree gchar *msg = g_strdup_printf ("Could not save project: %s", error->message);

      oe_log (OE_LOG_LEVEL_WARNING, "project save failed for '%s': %s", path, error->message);
      set_status_message (self, msg);
      g_error_free (error);
      return;
    }

  /* Only a successful save moves the on-disk anchor: a failed save
   * leaves any previous file byte-identical, so the anchor must not
   * move either. */
  g_clear_pointer (&self->project_path, g_free);
  self->project_path = g_strdup (path);

  g_autofree gchar *msg = g_strdup_printf ("Project saved to %s", path);

  set_status_message (self, msg);
}

static void
open_project_path (OeMainWindow *self, const gchar *path)
{
  GError *error = NULL;
  OeProject *loaded = oe_project_format_load (path, &error);

  if (loaded == NULL)
    {
      g_autofree gchar *msg = g_strdup_printf ("Could not open project: %s", error->message);

      oe_log (OE_LOG_LEVEL_WARNING, "project open failed for '%s': %s", path, error->message);
      set_status_message (self, msg);
      g_error_free (error);
      return;
    }

  /* Strict parse succeeded: replace the session, then re-import every
   * referenced path through the existing worker. Probe verdicts land as
   * OK / MISSING / UNSUPPORTED rows with the usual relink flow; the
   * model itself is already complete — clips reference project media
   * refs, not session assets. */
  reset_session (self, loaded);
  self->project_path = g_strdup (path);

  guint media_count = oe_project_get_media_count (self->project);

  for (guint i = 0; i < media_count; i++)
    {
      guint ref = 0;
      gchar *media_path = NULL;

      if (!oe_project_get_media (self->project, i, &ref, &media_path))
        continue;

      guint id = oe_media_library_add (self->media_library, media_path);

      register_media_asset_pair (self, ref, id);
      oe_import_worker_submit (self->import_worker, media_path, id, FALSE,
                               GUINT_TO_POINTER (self->session_epoch));
      self->import_pending++;
      self->import_batch_open = TRUE;
      g_free (media_path);
    }

  g_autofree gchar *msg
      = g_strdup_printf ("Loaded %s (%u tracks, %u media)", oe_project_get_name (self->project),
                         oe_project_get_track_count (self->project), media_count);

  set_status_message (self, msg);
}

/* The project picker filter mirrors add_media_filter: it only narrows
 * the chooser; the strict parser remains the accept/reject authority. */
static void
add_project_filter (GtkFileDialog *dialog)
{
  GtkFileFilter *filter = gtk_file_filter_new ();

  gtk_file_filter_set_name (filter, "Obvious Edit projects (*.oe)");
  gtk_file_filter_add_pattern (filter, "*.oe");

  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  g_object_unref (filters);
  g_object_unref (filter);
}

static void
on_open_project_dialog_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);
  GError *error = NULL;

  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file == NULL)
    {
      /* A canceled chooser is a no-op; real failures surface. */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          oe_log (OE_LOG_LEVEL_WARNING, "open project dialog failed: %s", error->message);
          set_status_message (self, "Could not open the file chooser");
        }
      g_clear_error (&error);
      return;
    }

  const gchar *path = g_file_peek_path (file);

  if (path != NULL)
    open_project_path (self, path);
  g_object_unref (file);
}

static void
project_open_command_handler (OeCommandId id G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (command_owner == NULL)
    return;

  GtkFileDialog *dialog = gtk_file_dialog_new ();

  gtk_file_dialog_set_title (dialog, "Open Project");
  add_project_filter (dialog);
  gtk_file_dialog_open (dialog, GTK_WINDOW (command_owner), NULL, on_open_project_dialog_done,
                        command_owner);
  g_object_unref (dialog);
}

static void
on_save_dialog_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);
  GError *error = NULL;

  GFile *file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          oe_log (OE_LOG_LEVEL_WARNING, "save project dialog failed: %s", error->message);
          set_status_message (self, "Could not open the file chooser");
        }
      g_clear_error (&error);
      return;
    }

  const gchar *path = g_file_peek_path (file);

  if (path != NULL)
    save_project_to_path (self, path);
  g_object_unref (file);
}

static void
project_save_command_handler (OeCommandId id G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (command_owner == NULL)
    return;

  /* Saving to the current anchor goes straight to disk; without one the
   * command turns into Save As. */
  if (command_owner->project_path != NULL)
    {
      save_project_to_path (command_owner, command_owner->project_path);
      return;
    }

  GtkFileDialog *dialog = gtk_file_dialog_new ();
  g_autofree gchar *initial
      = g_strdup_printf ("%s.oe", oe_project_get_name (command_owner->project));

  gtk_file_dialog_set_title (dialog, "Save Project");
  add_project_filter (dialog);
  gtk_file_dialog_set_initial_name (dialog, initial);
  gtk_file_dialog_save (dialog, GTK_WINDOW (command_owner), NULL, on_save_dialog_done,
                        command_owner);
  g_object_unref (dialog);
}

static void
project_new_command_handler (OeCommandId id G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (command_owner == NULL)
    return;

  reset_session (command_owner, oe_project_new_default ());
  set_status_message (command_owner, "New project started");
}

/* ------------------------------------------------------------------ */
/* Phase 4: timeline commands and the widget's resolve/report seams.   */
/* ------------------------------------------------------------------ */

/* Registers the session mapping between a project media reference
 * (stable, serialized) and the bin asset row that probed its file
 * (session-transient). */
static void
register_media_asset_pair (OeMainWindow *self, guint media_ref, guint asset_id)
{
  g_hash_table_insert (self->media_ref_to_asset, GUINT_TO_POINTER (media_ref),
                       GUINT_TO_POINTER (asset_id));
  g_hash_table_insert (self->asset_to_media_ref, GUINT_TO_POINTER (asset_id),
                       GUINT_TO_POINTER (media_ref));
}

static guint
lookup_asset_for_media_ref (OeMainWindow *self, guint media_ref)
{
  gpointer value = NULL;

  if (!g_hash_table_lookup_extended (self->media_ref_to_asset, GUINT_TO_POINTER (media_ref), NULL,
                                     &value))
    return 0;

  return GPOINTER_TO_UINT (value);
}

static guint
lookup_media_ref_for_asset (OeMainWindow *self, guint asset_id)
{
  gpointer value = NULL;

  if (!g_hash_table_lookup_extended (self->asset_to_media_ref, GUINT_TO_POINTER (asset_id), NULL,
                                     &value))
    return 0;

  return GPOINTER_TO_UINT (value);
}

/* Returns the existing project media reference for @path, or 0 when the
 * path is not referenced yet (oe_project_add_media always allocates a
 * fresh ref, so reuse is decided here, at the session layer). */
static guint
find_media_ref_by_path (OeMainWindow *self, const gchar *path)
{
  guint count = oe_project_get_media_count (self->project);

  for (guint i = 0; i < count; i++)
    {
      guint ref = 0;
      gchar *media_path = NULL;

      if (!oe_project_get_media (self->project, i, &ref, &media_path))
        continue;

      const gboolean match = g_strcmp0 (media_path, path) == 0;

      g_free (media_path);
      if (match)
        return ref;
    }

  return 0;
}

/* Resolve seam: the widget asks about one media reference per clip per
 * frame. Answers come from the library's session records through the
 * ref→asset map — no probing happens during draws. Unknown refs answer
 * missing, which renders the clip hatched. */
static void
timeline_resolve_media (guint media_ref, OeTimelineMediaInfo *info, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);
  OeAssetInfo asset;

  info->missing = TRUE;
  info->has_audio = FALSE;
  info->is_still = FALSE;

  if (lookup_asset_for_media_ref (self, media_ref) == 0)
    return;

  oe_asset_info_init (&asset);
  if (!oe_media_library_get (self->media_library, lookup_asset_for_media_ref (self, media_ref),
                             &asset))
    return;

  if (asset.status == OE_ASSET_STATUS_OK)
    {
      info->missing = FALSE;
      info->is_still = asset.info.kind == OE_MEDIA_KIND_STILL_IMAGE;
      info->has_audio = asset.info.channels > 0 && asset.info.sample_rate > 0;
    }

  oe_asset_info_clear (&asset);
}

/* Report seam: typed model rejections and missing-media refusals land
 * in the status bar, like every other command surface. */
static void
timeline_report (const gchar *message, gpointer user_data)
{
  set_status_message (OE_MAIN_WINDOW (user_data), message);
}

static void
view_zoom_in_command_handler (OeCommandId id G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (command_owner == NULL)
    return;

  oe_timeline_zoom_in (OE_TIMELINE (command_owner->timeline));
}

static void
view_zoom_out_command_handler (OeCommandId id G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (command_owner == NULL)
    return;

  oe_timeline_zoom_out (OE_TIMELINE (command_owner->timeline));
}

/* Fulfills the Phase 1 registry promise for selection.delete. */
static void
selection_delete_command_handler (OeCommandId id G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (command_owner == NULL)
    return;

  OeMainWindow *self = command_owner;
  guint track_index = 0;
  guint clip_index = 0;

  if (!oe_timeline_get_selection (OE_TIMELINE (self->timeline), &track_index, &clip_index))
    {
      set_status_message (self, "Delete: no clip selected");
      return;
    }

  GError *error = NULL;

  if (!oe_project_remove_clip (self->project, track_index, clip_index, &error))
    {
      g_autofree gchar *msg = g_strdup_printf ("Delete rejected: %s", error->message);

      set_status_message (self, msg);
      g_error_free (error);
      return;
    }

  oe_timeline_clear_selection (OE_TIMELINE (self->timeline));
  set_status_message (self, "Deleted selected clip");
}

/* Insert from Bin: the selected asset's file joins the project model
 * (one stable media ref per unique path) and a clip lands at the
 * playhead on the first kind-matching track. */
static void
media_insert_from_bin_command_handler (OeCommandId id G_GNUC_UNUSED,
                                       gpointer user_data G_GNUC_UNUSED)
{
  if (command_owner == NULL)
    return;

  OeMainWindow *self = command_owner;
  const guint asset_id = oe_media_bin_get_selected (OE_MEDIA_BIN (self->media_bin));

  if (asset_id == 0)
    {
      set_status_message (self, "Insert from Bin: select an asset in the bin first");
      return;
    }

  OeAssetInfo asset;

  oe_asset_info_init (&asset);
  if (!oe_media_library_get (self->media_library, asset_id, &asset))
    return;

  if (asset.status != OE_ASSET_STATUS_OK)
    {
      g_autofree gchar *msg = g_strdup_printf ("Insert from Bin: '%s' is not ready (%s)",
                                               asset.name, oe_asset_status_get_name (asset.status));

      set_status_message (self, msg);
      oe_asset_info_clear (&asset);
      return;
    }

  /* Screen duration for stills (uniform-duration rule: the source
   * range encodes screen time); probed length for AV media. */
  const gint64 duration_us = asset.info.kind == OE_MEDIA_KIND_STILL_IMAGE
                                 ? (gint64) OE_TIMELINE_DEFAULT_STILL_US
                                 : asset.info.duration_us;

  if (duration_us <= 0)
    {
      g_autofree gchar *msg
          = g_strdup_printf ("Insert from Bin: '%s' has no usable duration", asset.name);

      set_status_message (self, msg);
      oe_asset_info_clear (&asset);
      return;
    }

  guint media_ref = find_media_ref_by_path (self, asset.path);

  if (media_ref == 0)
    {
      media_ref = oe_project_add_media (self->project, asset.path);
      register_media_asset_pair (self, media_ref, asset_id);
      oe_project_set_media_source_duration (self->project, media_ref, duration_us);
    }

  /* Destination: the first kind-matching track, from a fresh deep
   * copy (track kinds are sequence data). */
  const OeTrackKind want = asset.info.kind == OE_MEDIA_KIND_AUDIO ? OE_TRACK_AUDIO : OE_TRACK_VIDEO;
  OeSequence sequence;
  guint track_index = G_MAXUINT;

  oe_sequence_init (&sequence);
  oe_project_get_sequence (self->project, &sequence);

  for (guint i = 0; i < sequence.tracks->len; i++)
    {
      const OeTrack *track = g_ptr_array_index (sequence.tracks, i);

      if (track->kind == want)
        {
          track_index = i;
          break;
        }
    }

  oe_sequence_clear (&sequence);

  if (track_index == G_MAXUINT)
    {
      set_status_message (self, want == OE_TRACK_AUDIO
                                    ? "Insert from Bin: no audio track in the sequence"
                                    : "Insert from Bin: no video track in the sequence");
      oe_asset_info_clear (&asset);
      return;
    }

  const gint64 playhead_us = oe_timeline_get_playhead (OE_TIMELINE (self->timeline));
  GError *error = NULL;

  if (!oe_project_insert_clip (self->project, track_index, media_ref, playhead_us, 0, duration_us,
                               &error))
    {
      g_autofree gchar *msg = g_strdup_printf ("Insert rejected: %s", error->message);

      set_status_message (self, msg);
      g_error_free (error);
      oe_asset_info_clear (&asset);
      return;
    }

  /* The playhead advances past the inserted clip so consecutive
   * inserts stack; session-only state (Phase 5 owns the clock). */
  oe_timeline_set_playhead (OE_TIMELINE (self->timeline), playhead_us + duration_us);

  g_autofree gchar *msg
      = g_strdup_printf ("Inserted '%s' on track %u at the playhead", asset.name, track_index);

  set_status_message (self, msg);
  oe_asset_info_clear (&asset);
}

/* ------------------------------------------------------------------ */
/* Panels: labeled frames with a phase-aware empty state.              */
/* ------------------------------------------------------------------ */

static GtkWidget *
panel_new (const gchar *title, const gchar *empty_text)
{
  GtkWidget *panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (panel, "panel");

  GtkWidget *header = gtk_label_new (title);

  gtk_widget_add_css_class (header, "panel-title");
  gtk_widget_set_halign (header, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (panel), header);

  GtkWidget *body = gtk_label_new (empty_text);

  gtk_label_set_wrap (GTK_LABEL (body), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (body), 24);
  gtk_widget_add_css_class (body, "empty-state");
  gtk_widget_set_halign (body, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (body, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (body, TRUE);
  gtk_widget_set_vexpand (body, TRUE);
  gtk_box_append (GTK_BOX (panel), body);

  return panel;
}

/* The inspector is a panel whose body swaps between the preserved empty
 * state and the probed-record grid. */
static GtkWidget *
inspector_panel_new (GtkWidget **stack_out, GtkWidget **grid_out)
{
  GtkWidget *panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (panel, "panel");

  GtkWidget *header = gtk_label_new ("Inspector");

  gtk_widget_add_css_class (header, "panel-title");
  gtk_widget_set_halign (header, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (panel), header);

  GtkWidget *stack = gtk_stack_new ();

  gtk_widget_set_vexpand (stack, TRUE);

  GtkWidget *empty = gtk_label_new ("No selection — properties appear here");

  gtk_label_set_wrap (GTK_LABEL (empty), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (empty), 24);
  gtk_widget_add_css_class (empty, "empty-state");
  gtk_widget_set_halign (empty, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (empty, GTK_ALIGN_CENTER);
  gtk_stack_add_named (GTK_STACK (stack), empty, "empty");

  GtkWidget *scroller = gtk_scrolled_window_new ();

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  GtkWidget *grid = gtk_grid_new ();

  gtk_grid_set_row_spacing (GTK_GRID (grid), 4);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_widget_set_margin_top (grid, 8);
  gtk_widget_set_margin_bottom (grid, 8);
  gtk_widget_set_margin_start (grid, 10);
  gtk_widget_set_margin_end (grid, 10);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), grid);
  gtk_stack_add_named (GTK_STACK (stack), scroller, "media");

  gtk_stack_set_visible_child_name (GTK_STACK (stack), "empty");
  gtk_box_append (GTK_BOX (panel), stack);

  *stack_out = stack;
  *grid_out = grid;
  return panel;
}

/* ------------------------------------------------------------------ */
/* Menu bar: every item routes through an app.<command> action.        */
/* ------------------------------------------------------------------ */

static void
menu_add_command (GMenu *menu, const gchar *label, const gchar *command_name)
{
  g_autofree gchar *action = g_strdup_printf ("app.%s", command_name);

  g_menu_append (menu, label, action);
}

static GtkWidget *
build_menu_bar (void)
{
  GMenu *transport = g_menu_new ();

  menu_add_command (transport, "Shuttle Back (J)", "transport.shuttle-back");
  menu_add_command (transport, "Play / Pause (Space)", "transport.play-pause");
  menu_add_command (transport, "Stop (K)", "transport.stop");
  menu_add_command (transport, "Shuttle Forward (L)", "transport.shuttle-forward");
  menu_add_command (transport, "Mark In (I)", "transport.mark-in");
  menu_add_command (transport, "Mark Out (O)", "transport.mark-out");

  GMenu *edit = g_menu_new ();

  menu_add_command (edit, "Undo", "edit.undo");
  menu_add_command (edit, "Redo", "edit.redo");
  menu_add_command (edit, "Insert from Bin (Ctrl+E)", "media.insert-from-bin");
  menu_add_command (edit, "Delete Selection", "selection.delete");
  menu_add_command (edit, "Select Tool (V)", "tool.select");
  menu_add_command (edit, "Razor Tool (C)", "tool.razor");

  /* Phase 4: zoom is view session state; the timeline widget clamps
   * around the anchor (widget center for commands, pointer for
   * Ctrl+wheel). */
  GMenu *view = g_menu_new ();

  menu_add_command (view, "Zoom In (Ctrl+=)", "view.zoom-in");
  menu_add_command (view, "Zoom Out (Ctrl+-)", "view.zoom-out");

  GMenu *file = g_menu_new ();

  menu_add_command (file, "New Project", "project.new");
  menu_add_command (file, "Open Project…", "project.open");
  menu_add_command (file, "Save Project", "project.save");
  menu_add_command (file, "Import Media…", "media.import");

  GMenu *help = g_menu_new ();

  menu_add_command (help, "About Obvious Edit", "help.about");

  GMenu *menubar = g_menu_new ();

  g_menu_append_submenu (menubar, "File", G_MENU_MODEL (file));
  g_menu_append_submenu (menubar, "Edit", G_MENU_MODEL (edit));
  g_menu_append_submenu (menubar, "View", G_MENU_MODEL (view));
  g_menu_append_submenu (menubar, "Transport", G_MENU_MODEL (transport));
  g_menu_append_submenu (menubar, "Help", G_MENU_MODEL (help));

  GtkWidget *bar = gtk_popover_menu_bar_new_from_model (G_MENU_MODEL (menubar));

  g_object_unref (menubar);
  return bar;
}

/* ------------------------------------------------------------------ */
/* Toolbar: clickable stand-ins for the same app.<command> actions.    */
/* ------------------------------------------------------------------ */

static GtkWidget *
toolbar_button (const gchar *label, const gchar *command_name, const gchar *tooltip)
{
  GtkWidget *button = gtk_button_new_with_label (label);
  g_autofree gchar *action = g_strdup_printf ("app.%s", command_name);

  gtk_actionable_set_action_name (GTK_ACTIONABLE (button), action);
  gtk_widget_set_tooltip_text (button, tooltip);
  return button;
}

static GtkWidget *
toolbar_separator (void)
{
  GtkWidget *separator = gtk_separator_new (GTK_ORIENTATION_VERTICAL);

  gtk_widget_set_margin_top (separator, 2);
  gtk_widget_set_margin_bottom (separator, 2);
  return separator;
}

static GtkWidget *
build_toolbar (void)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

  gtk_widget_add_css_class (bar, "toolbar");
  gtk_widget_set_hexpand (bar, TRUE);

  gtk_box_append (GTK_BOX (bar), toolbar_button ("New", "project.new", "New project"));
  gtk_box_append (GTK_BOX (bar), toolbar_button ("Open", "project.open", "Open project"));
  gtk_box_append (GTK_BOX (bar), toolbar_button ("Save", "project.save", "Save project"));
  gtk_box_append (GTK_BOX (bar),
                  toolbar_button ("Import…", "media.import", "Import media (Ctrl+I)"));
  gtk_box_append (GTK_BOX (bar), toolbar_separator ());
  gtk_box_append (GTK_BOX (bar), toolbar_button ("Undo", "edit.undo", "Undo (Ctrl+Z)"));
  gtk_box_append (GTK_BOX (bar), toolbar_button ("Redo", "edit.redo", "Redo (Ctrl+Shift+Z)"));
  gtk_box_append (GTK_BOX (bar), toolbar_separator ());
  gtk_box_append (GTK_BOX (bar),
                  toolbar_button ("Play", "transport.play-pause", "Play / Pause (Space)"));
  gtk_box_append (GTK_BOX (bar), toolbar_button ("Stop", "transport.stop", "Stop (K)"));
  gtk_box_append (GTK_BOX (bar),
                  toolbar_button ("Mark In", "transport.mark-in", "Mark in point (I)"));
  gtk_box_append (GTK_BOX (bar),
                  toolbar_button ("Mark Out", "transport.mark-out", "Mark out point (O)"));

  return bar;
}

/* Transport row docked inside the timeline area. */
static GtkWidget *
build_transport_row (void)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

  gtk_widget_add_css_class (row, "transport");
  gtk_widget_set_halign (row, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (row),
                  toolbar_button ("|◀◀", "transport.shuttle-back", "Shuttle back (J)"));
  gtk_box_append (GTK_BOX (row),
                  toolbar_button ("Play", "transport.play-pause", "Play / Pause (Space)"));
  gtk_box_append (GTK_BOX (row), toolbar_button ("Stop", "transport.stop", "Stop (K)"));
  gtk_box_append (GTK_BOX (row),
                  toolbar_button ("▶▶|", "transport.shuttle-forward", "Shuttle forward (L)"));
  gtk_box_append (GTK_BOX (row),
                  toolbar_button ("Mark In", "transport.mark-in", "Mark in point (I)"));
  gtk_box_append (GTK_BOX (row),
                  toolbar_button ("Mark Out", "transport.mark-out", "Mark out point (O)"));

  return row;
}

/* ------------------------------------------------------------------ */
/* Layout persistence: load at construction, apply on map, save on close. */
/* ------------------------------------------------------------------ */

static void
on_map_apply_positions (GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
  OeMainWindow *self = OE_MAIN_WINDOW (widget);

  if (self->positions_applied)
    return;

  self->positions_applied = TRUE;
  gtk_paned_set_position (GTK_PANED (self->bin_paned), self->pending_bin);
  gtk_paned_set_position (GTK_PANED (self->inspector_paned), self->pending_inspector);
  gtk_paned_set_position (GTK_PANED (self->timeline_paned), self->pending_timeline);
}

/* The timeline panel: labeled chrome, transport row, and the live
 * OeTimeline body. The truthful empty states (no tracks / empty
 * tracks) are painted by the widget itself, replacing the Phase 3
 * placeholder string. */
static GtkWidget *
timeline_panel_new (GtkWidget **timeline_out)
{
  GtkWidget *panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (panel, "panel");

  GtkWidget *header = gtk_label_new ("Timeline");

  gtk_widget_add_css_class (header, "panel-title");
  gtk_widget_set_halign (header, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (panel), header);

  GtkWidget *timeline = GTK_WIDGET (oe_timeline_new ());

  gtk_widget_set_hexpand (timeline, TRUE);
  gtk_widget_set_vexpand (timeline, TRUE);
  gtk_box_append (GTK_BOX (panel), timeline);
  gtk_box_append (GTK_BOX (panel), build_transport_row ());

  *timeline_out = timeline;
  return panel;
}

static void
on_close_request (OeMainWindow *self, gpointer user_data G_GNUC_UNUSED)
{
  OeShellLayout layout;
  gint bin_position;
  gint inspector_position;
  gint timeline_position;
  GError *error = NULL;

  if (self->layout_saved)
    return;

  self->layout_saved = TRUE;

  oe_shell_layout_defaults (&layout);

  /* gtk_widget_get_width/height is the GTK4 replacement for the removed
   * gtk_window_get_size; at close time the window has a live allocation. */
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  int height = gtk_widget_get_height (GTK_WIDGET (self));

  if (width > 0 && height > 0)
    {
      layout.window_width = width;
      layout.window_height = height;
    }
  layout.window_maximized = gtk_window_is_maximized (GTK_WINDOW (self));

  bin_position = gtk_paned_get_position (GTK_PANED (self->bin_paned));
  inspector_position = gtk_paned_get_position (GTK_PANED (self->inspector_paned));
  timeline_position = gtk_paned_get_position (GTK_PANED (self->timeline_paned));

  /* -1 means the pane was never allocated; keep the default then. */
  if (bin_position >= 0)
    layout.bin_width = bin_position;
  if (inspector_position >= 0)
    layout.inspector_width = inspector_position;
  if (timeline_position >= 0)
    layout.timeline_height = timeline_position;

  if (!oe_shell_layout_save (&layout, &error))
    {
      oe_log (OE_LOG_LEVEL_WARNING, "layout save failed: %s", error->message);
      g_clear_error (&error);
    }
}

static void
oe_main_window_dispose (GObject *object)
{
  OeMainWindow *self = OE_MAIN_WINDOW (object);

  /* Both seams point back at this window: clear them before any child
   * widget dies so a late dispatch or drop can never touch a dangling
   * widget. */
  oe_command_set_reporter (NULL, NULL);
  oe_command_set_handler (OE_CMD_IMPORT_MEDIA, NULL);
  oe_command_set_handler (OE_CMD_NEW_PROJECT, NULL);
  oe_command_set_handler (OE_CMD_OPEN_PROJECT, NULL);
  oe_command_set_handler (OE_CMD_SAVE_PROJECT, NULL);
  oe_command_set_handler (OE_CMD_IMPORT_FROM_BIN, NULL);
  oe_command_set_handler (OE_CMD_DELETE_SELECTION, NULL);
  oe_command_set_handler (OE_CMD_ZOOM_IN, NULL);
  oe_command_set_handler (OE_CMD_ZOOM_OUT, NULL);
  command_owner = NULL;

  /* Free the worker first: it drains, joins, and flushes pending
   * results onto the main context while the library and widgets it
   * reports about are still alive. Runs BEFORE oe_ffmpeg_shutdown,
   * which the application performs in its own teardown. */
  g_clear_pointer (&self->import_worker, oe_import_worker_free);
  g_clear_pointer (&self->media_library, oe_media_library_free);

  /* The timeline observes the project through a weak pointer: detach
   * it before the model dies so the observer can never fire into a
   * widget that is mid-teardown (or vice versa). */
  if (self->timeline != NULL)
    oe_timeline_set_project (OE_TIMELINE (self->timeline), NULL);

  /* The model dies last: the worker is drained and both seams are
   * cleared above, so nothing can reference it during teardown. */
  g_clear_pointer (&self->project_path, g_free);
  g_clear_object (&self->project);

  g_clear_pointer (&self->media_ref_to_asset, g_hash_table_unref);
  g_clear_pointer (&self->asset_to_media_ref, g_hash_table_unref);

  G_OBJECT_CLASS (oe_main_window_parent_class)->dispose (object);
}

static void
on_library_changed (guint asset_id G_GNUC_UNUSED, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);

  oe_media_bin_refresh (OE_MEDIA_BIN (self->media_bin));
}

static void
oe_main_window_constructed (GObject *object)
{
  OeMainWindow *self = OE_MAIN_WINDOW (object);
  OeShellLayout layout;

  G_OBJECT_CLASS (oe_main_window_parent_class)->constructed (object);

  /* Restore the persisted window size and panel positions (documented
   * defaults on first launch). */
  oe_shell_layout_load (&layout, NULL);
  gtk_window_set_title (GTK_WINDOW (self), "Obvious Edit");
  gtk_window_set_default_size (GTK_WINDOW (self), layout.window_width, layout.window_height);
  if (layout.window_maximized)
    gtk_window_maximize (GTK_WINDOW (self));

  self->pending_bin = layout.bin_width;
  self->pending_inspector = layout.inspector_width;
  self->pending_timeline = layout.timeline_height;

  /* Phase 2 services: the window owns the GTK-free library and worker;
   * the bin is a projection of the library. Phase 3 adds the GTK-free
   * project model the New/Open/Save commands act on. */
  self->media_library = oe_media_library_new ();
  self->project = oe_project_new_default ();
  self->media_bin = GTK_WIDGET (oe_media_bin_new (self->media_library));
  self->import_worker = oe_import_worker_new (on_import_done, self);

  oe_media_library_set_observer (self->media_library, on_library_changed, self);
  oe_media_bin_set_import_func (OE_MEDIA_BIN (self->media_bin), bin_import_sink, self);
  g_signal_connect (self->media_bin, "selection-changed", G_CALLBACK (on_bin_selection_changed),
                    self);
  g_signal_connect (self->media_bin, "relink-requested", G_CALLBACK (on_relink_requested), self);

  /* Workspace composition. Nesting, outside in:
   *   window
   *    +- menu bar, toolbar, status bar
   *    +- bin_paned (horizontal)
   *        +- media bin
   *        +- timeline_paned (vertical)
   *            +- inspector_paned (horizontal)
   *            |   +- monitors box (source | program)
   *            |   +- inspector
   *            +- timeline area (+ transport)
   */
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *menu_bar = build_menu_bar ();

  gtk_box_append (GTK_BOX (root), menu_bar);
  gtk_box_append (GTK_BOX (root), build_toolbar ());

  self->bin_paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_shrink_start_child (GTK_PANED (self->bin_paned), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (self->bin_paned), FALSE);
  gtk_paned_set_start_child (GTK_PANED (self->bin_paned), self->media_bin);

  self->inspector_paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_shrink_start_child (GTK_PANED (self->inspector_paned), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (self->inspector_paned), FALSE);

  GtkWidget *monitors = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_box_set_homogeneous (GTK_BOX (monitors), TRUE);
  gtk_box_append (GTK_BOX (monitors),
                  panel_new ("Source Monitor",
                             "No clip loaded yet — source playback arrives in a later phase"));
  gtk_box_append (GTK_BOX (monitors),
                  panel_new ("Program Monitor",
                             "No video output yet — program display arrives in a later phase"));

  gtk_paned_set_start_child (GTK_PANED (self->inspector_paned), monitors);
  gtk_paned_set_end_child (GTK_PANED (self->inspector_paned),
                           inspector_panel_new (&self->inspector_stack, &self->inspector_media));

  self->timeline_paned = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_shrink_start_child (GTK_PANED (self->timeline_paned), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (self->timeline_paned), FALSE);
  gtk_paned_set_start_child (GTK_PANED (self->timeline_paned), self->inspector_paned);

  GtkWidget *timeline = timeline_panel_new (&self->timeline);

  gtk_paned_set_end_child (GTK_PANED (self->timeline_paned), timeline);

  /* The widget is the first production observer consumer: it redraws
   * from deep copies on every project notification and answers
   * missing/kind questions through the session's resolve seam. */
  oe_timeline_set_resolve_func (OE_TIMELINE (self->timeline), timeline_resolve_media, self);
  oe_timeline_set_report_func (OE_TIMELINE (self->timeline), timeline_report, self);
  oe_timeline_set_project (OE_TIMELINE (self->timeline), self->project);

  gtk_paned_set_end_child (GTK_PANED (self->bin_paned), self->timeline_paned);
  gtk_box_append (GTK_BOX (root), self->bin_paned);

  /* A plain styled label instead of GtkStatusBar (deprecated in GTK 4.18). */
  self->status_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->status_bar, "statusbar");
  self->status_label
      = gtk_label_new ("Ready — commands report here until later phases implement them");
  gtk_widget_set_halign (GTK_WIDGET (self->status_label), GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (self->status_bar), self->status_label);
  gtk_box_append (GTK_BOX (root), self->status_bar);

  gtk_window_set_child (GTK_WINDOW (self), root);

  /* Dispatch feedback lands in the status bar; media.import and the
   * project commands land here. */
  oe_command_set_reporter (report_to_status_bar, self);
  command_owner = self;
  oe_command_set_handler (OE_CMD_IMPORT_MEDIA, media_import_command_handler);
  oe_command_set_handler (OE_CMD_NEW_PROJECT, project_new_command_handler);
  oe_command_set_handler (OE_CMD_OPEN_PROJECT, project_open_command_handler);
  oe_command_set_handler (OE_CMD_SAVE_PROJECT, project_save_command_handler);
  oe_command_set_handler (OE_CMD_IMPORT_FROM_BIN, media_insert_from_bin_command_handler);
  oe_command_set_handler (OE_CMD_DELETE_SELECTION, selection_delete_command_handler);
  oe_command_set_handler (OE_CMD_ZOOM_IN, view_zoom_in_command_handler);
  oe_command_set_handler (OE_CMD_ZOOM_OUT, view_zoom_out_command_handler);

  g_signal_connect (self, "map", G_CALLBACK (on_map_apply_positions), NULL);
  g_signal_connect (self, "close-request", G_CALLBACK (on_close_request), NULL);

  oe_log (OE_LOG_LEVEL_INFO, "editor shell built (%dx%d default)", layout.window_width,
          layout.window_height);
}

static void
oe_main_window_init (OeMainWindow *self)
{
  self->positions_applied = FALSE;
  self->layout_saved = FALSE;
  self->import_pending = 0;
  self->import_ok = 0;
  self->relink_target = 0;
  self->session_epoch = 1; /* stamps worker jobs; never 0 so NULL tags fail */

  /* Phase 4: ref↔asset pairs, rebuilt per session by reset_session. */
  self->media_ref_to_asset = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->asset_to_media_ref = g_hash_table_new (g_direct_hash, g_direct_equal);
}

static void
oe_main_window_class_init (OeMainWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->constructed = oe_main_window_constructed;
  G_OBJECT_CLASS (klass)->dispose = oe_main_window_dispose;
}

GtkWidget *
oe_main_window_new (GtkApplication *application)
{
  g_return_val_if_fail (GTK_IS_APPLICATION (application), NULL);

  oe_log (OE_LOG_LEVEL_DEBUG, "main window created");
  return GTK_WIDGET (g_object_new (OE_TYPE_MAIN_WINDOW, "application", application, NULL));
}
