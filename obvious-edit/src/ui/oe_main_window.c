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
 */

#include "oe_main_window.h"

#include <gtk/gtk.h>

#include "../app/oe_command.h"
#include "../app/oe_import_worker.h"
#include "../app/oe_log.h"
#include "../app/oe_media_library.h"
#include "oe_media_bin.h"
#include "oe_shell_layout.h"

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
/* Static handler seam: media.import carries no user_data through      */
/* dispatch, so the handler context rides a static pointer, installed  */
/* at construction and cleared in dispose — the same pattern as the    */
/* reporter seam.                                                      */
/* ------------------------------------------------------------------ */

static OeMainWindow *media_import_owner = NULL;

/* ------------------------------------------------------------------ */
/* Status reporting: every user-visible phase-2 message lands through   */
/* the same status bar the command reporter writes to.                  */
/* ------------------------------------------------------------------ */

/* Forward decls: the import callback repopulates the inspector before
 * the inspector section below defines it. */
static void populate_inspector (OeMainWindow *self);

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
  for (guint i = 0; paths != NULL && paths[i] != NULL; i++)
    {
      guint id = oe_media_library_add (self->media_library, paths[i]);

      /* The observer refreshed the bin (IMPORTING row); the worker's
       * verdict will move the record to OK, MISSING, or UNSUPPORTED. */
      oe_import_worker_submit (self->import_worker, paths[i], id, FALSE);
      self->import_pending++;
    }
}

static void
on_import_done (const OeImportJobResult *result, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);

  switch (result->result)
    {
    case OE_IMPORT_RESULT_OK:
      oe_media_library_mark_ok (self->media_library, result->asset_id, &result->info);
      oe_media_library_set_thumbnail (self->media_library, result->asset_id, &result->thumbnail);
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

  if (self->import_pending == 0 && self->import_ok > 0)
    {
      g_autofree gchar *msg = g_strdup_printf ("Imported %u file(s)", self->import_ok);

      set_status_message (self, msg);
      self->import_ok = 0;
    }

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
      oe_import_worker_submit (self->import_worker, path, target, TRUE);
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
  if (media_import_owner != NULL)
    open_import_dialog (media_import_owner);
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
  menu_add_command (edit, "Delete Selection", "selection.delete");
  menu_add_command (edit, "Select Tool (V)", "tool.select");
  menu_add_command (edit, "Razor Tool (C)", "tool.razor");

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
  media_import_owner = NULL;

  /* Free the worker first: it drains, joins, and flushes pending
   * results onto the main context while the library and widgets it
   * reports about are still alive. Runs BEFORE oe_ffmpeg_shutdown,
   * which the application performs in its own teardown. */
  g_clear_pointer (&self->import_worker, oe_import_worker_free);
  g_clear_pointer (&self->media_library, oe_media_library_free);

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
   * the bin is a projection of the library. */
  self->media_library = oe_media_library_new ();
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
                  panel_new ("Program Monitor", "No sequence loaded yet — Phase 3"));

  gtk_paned_set_start_child (GTK_PANED (self->inspector_paned), monitors);
  gtk_paned_set_end_child (GTK_PANED (self->inspector_paned),
                           inspector_panel_new (&self->inspector_stack, &self->inspector_media));

  self->timeline_paned = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_shrink_start_child (GTK_PANED (self->timeline_paned), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (self->timeline_paned), FALSE);
  gtk_paned_set_start_child (GTK_PANED (self->timeline_paned), self->inspector_paned);

  GtkWidget *timeline = panel_new ("Timeline", "No sequence yet — Phase 3 adds the timeline model");

  gtk_box_append (GTK_BOX (timeline), build_transport_row ());
  gtk_paned_set_end_child (GTK_PANED (self->timeline_paned), timeline);

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

  /* Dispatch feedback lands in the status bar; media.import lands here. */
  oe_command_set_reporter (report_to_status_bar, self);
  media_import_owner = self;
  oe_command_set_handler (OE_CMD_IMPORT_MEDIA, media_import_command_handler);

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
