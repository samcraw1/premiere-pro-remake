/* oe_media_bin.c — the media bin panel implementation (Phase 2).
 *
 * Rows are rebuilt from the library on every refresh: session scale is
 * small, and rebuilding keeps the row state a pure projection of the
 * records (no incremental diffing to drift out of sync). Dropping files
 * forwards paths to the import entry point; this widget never decodes
 * and never touches the worker directly.
 */

#include "oe_media_bin.h"

#include "../app/oe_log.h"

enum
{
  SIGNAL_SELECTION_CHANGED,
  SIGNAL_RELINK_REQUESTED,
  N_SIGNALS,
};

static guint bin_signals[N_SIGNALS];

struct _OeMediaBin
{
  GtkWidget parent_instance;

  OeMediaLibrary *library; /* owned by the window */
  GtkWidget *stack;        /* "empty" | "list" */
  GtkWidget *list;         /* GtkListBox */

  OeMediaBinImportFunc import_func;
  gpointer import_data;

  guint selected_id;
};

G_DEFINE_TYPE (OeMediaBin, oe_media_bin, GTK_TYPE_WIDGET)

/* ------------------------------------------------------------------ */
/* Presentation helpers (pure; shared with the inspector).             */
/* ------------------------------------------------------------------ */

const gchar *
oe_media_bin_kind_name (OeMediaKind kind)
{
  switch (kind)
    {
    case OE_MEDIA_KIND_VIDEO:
      return "Video";
    case OE_MEDIA_KIND_AUDIO:
      return "Audio";
    case OE_MEDIA_KIND_STILL_IMAGE:
      return "Still Image";
    default:
      return "Unknown";
    }
}

/* Integer microseconds rendered as h:mm:ss.mmm — no floats (the
 * project-format time model floor). */
gchar *
oe_media_bin_format_duration_us (gint64 duration_us)
{
  if (duration_us < 0)
    duration_us = 0;

  gint64 seconds_total = duration_us / G_USEC_PER_SEC;
  gint64 millis = (duration_us % G_USEC_PER_SEC) / 1000;

  return g_strdup_printf (
      "%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT,
      seconds_total / 3600, (seconds_total / 60) % 60, seconds_total % 60, millis);
}

/* ------------------------------------------------------------------ */
/* Rows.                                                               */
/* ------------------------------------------------------------------ */

#define ROW_THUMB_SIZE 48

static GtkWidget *
thumbnail_image (const OeAssetInfo *info)
{
  GtkWidget *image = gtk_image_new ();

  gtk_widget_set_size_request (image, ROW_THUMB_SIZE, ROW_THUMB_SIZE);
  gtk_widget_set_valign (image, GTK_ALIGN_CENTER);

  if (info->thumbnail.rgba != NULL && info->thumbnail.width > 0 && info->thumbnail.height > 0)
    {
      gsize len = (gsize) info->thumbnail.width * info->thumbnail.height * 4;
      GBytes *bytes = g_bytes_new (info->thumbnail.rgba, len);

      /* GdkMemoryTexture keeps its own reference on @bytes. */
      GdkTexture *texture
          = gdk_memory_texture_new ((gsize) info->thumbnail.width, (gsize) info->thumbnail.height,
                                    GDK_MEMORY_R8G8B8A8, bytes, (gsize) info->thumbnail.width * 4);

      g_bytes_unref (bytes);
      gtk_image_set_from_paintable (GTK_IMAGE (image), GDK_PAINTABLE (texture));
      g_object_unref (texture);
      gtk_widget_add_css_class (image, "media-thumb");
    }
  else
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (image), "image-x-generic");
      gtk_widget_add_css_class (image, "media-thumb-placeholder");
    }

  return image;
}

static GtkWidget *
meta_line (const OeAssetInfo *info)
{
  g_autofree gchar *detail = NULL;

  if (info->info.kind == OE_MEDIA_KIND_STILL_IMAGE && info->info.width > 0)
    detail = g_strdup_printf ("%d×%d", info->info.width, info->info.height);
  else if (info->info.duration_us > 0)
    detail = oe_media_bin_format_duration_us (info->info.duration_us);
  else
    detail = g_strdup ("");

  g_autofree gchar *text
      = g_strdup_printf ("%s · %s", oe_media_bin_kind_name (info->info.kind), detail);

  return gtk_label_new (text);
}

static GtkWidget *
badge_label (OeAssetStatus status)
{
  GtkWidget *badge = gtk_label_new (oe_asset_status_get_name (status));

  gtk_widget_add_css_class (badge, "status-badge");
  switch (status)
    {
    case OE_ASSET_STATUS_IMPORTING:
      gtk_widget_add_css_class (badge, "badge-importing");
      break;
    case OE_ASSET_STATUS_OK:
      gtk_widget_add_css_class (badge, "badge-ok");
      break;
    case OE_ASSET_STATUS_MISSING:
      gtk_widget_add_css_class (badge, "badge-missing");
      break;
    case OE_ASSET_STATUS_UNSUPPORTED:
      gtk_widget_add_css_class (badge, "badge-unsupported");
      break;
    default:
      break;
    }
  return badge;
}

static void
on_relink_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  OeMediaBin *self = OE_MEDIA_BIN (user_data);

  g_signal_emit (self, bin_signals[SIGNAL_RELINK_REQUESTED], 0,
                 GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "asset-id")));
}

static GtkWidget *
build_row (OeMediaBin *self, const OeAssetInfo *info)
{
  GtkWidget *row = gtk_list_box_row_new ();

  gtk_widget_add_css_class (row, "media-bin-row");
  g_object_set_data (G_OBJECT (row), "asset-id", GUINT_TO_POINTER (info->id));

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_margin_top (box, 4);
  gtk_widget_set_margin_bottom (box, 4);
  gtk_widget_set_margin_start (box, 8);
  gtk_widget_set_margin_end (box, 8);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);

  gtk_box_append (GTK_BOX (box), thumbnail_image (info));

  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (text, TRUE);

  GtkWidget *name = gtk_label_new (info->name);

  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (name), 0.0);
  gtk_widget_add_css_class (name, "media-row-name");
  gtk_box_append (GTK_BOX (text), name);

  GtkWidget *meta = meta_line (info);

  gtk_label_set_xalign (GTK_LABEL (meta), 0.0);
  gtk_widget_add_css_class (meta, "media-row-meta");
  gtk_box_append (GTK_BOX (text), meta);

  gtk_box_append (GTK_BOX (box), text);
  gtk_box_append (GTK_BOX (box), badge_label (info->status));

  /* Only broken rows offer relinking — OK rows are connected, IMPORTING
   * rows are still in flight. */
  if (info->status == OE_ASSET_STATUS_MISSING || info->status == OE_ASSET_STATUS_UNSUPPORTED)
    {
      GtkWidget *relink = gtk_button_new_with_label ("Relink…");

      gtk_widget_set_valign (relink, GTK_ALIGN_CENTER);
      g_object_set_data (G_OBJECT (relink), "asset-id", GUINT_TO_POINTER (info->id));
      g_signal_connect (relink, "clicked", G_CALLBACK (on_relink_clicked), self);
      gtk_box_append (GTK_BOX (box), relink);
    }

  return row;
}

/* ------------------------------------------------------------------ */
/* Refresh + selection.                                                */
/* ------------------------------------------------------------------ */

void
oe_media_bin_refresh (OeMediaBin *bin)
{
  g_return_if_fail (OE_IS_MEDIA_BIN (bin));

  /* Rebuild: the bin is a projection of the library, never a second
   * copy of its state. */
  GtkWidget *child = gtk_widget_get_first_child (bin->list);

  while (child != NULL)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);

      gtk_list_box_remove (GTK_LIST_BOX (bin->list), child);
      child = next;
    }

  bin->selected_id = 0;

  GList *ids = oe_media_library_list_ids (bin->library);
  gboolean has_rows = ids != NULL;

  for (GList *iter = ids; iter != NULL; iter = iter->next)
    {
      OeAssetInfo info;

      if (!oe_media_library_get (bin->library, GPOINTER_TO_UINT (iter->data), &info))
        continue;

      gtk_list_box_append (GTK_LIST_BOX (bin->list), build_row (bin, &info));
      oe_asset_info_clear (&info);
    }
  g_list_free (ids);

  gtk_stack_set_visible_child_name (GTK_STACK (bin->stack), has_rows ? "list" : "empty");
}

guint
oe_media_bin_get_selected (OeMediaBin *bin)
{
  g_return_val_if_fail (OE_IS_MEDIA_BIN (bin), 0);

  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (bin->list));

  if (row == NULL)
    return 0;
  return GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "asset-id"));
}

static void
on_selection_changed (GtkListBox *list G_GNUC_UNUSED, gpointer user_data)
{
  OeMediaBin *self = OE_MEDIA_BIN (user_data);

  self->selected_id = oe_media_bin_get_selected (self);
  g_signal_emit (self, bin_signals[SIGNAL_SELECTION_CHANGED], 0);
}

/* ------------------------------------------------------------------ */
/* Drag and drop: GDK_TYPE_FILE_LIST → the import entry point.         */
/* ------------------------------------------------------------------ */

static gboolean
on_drop (GtkDropTarget *target G_GNUC_UNUSED, const GValue *value, gdouble x G_GNUC_UNUSED,
         gdouble y G_GNUC_UNUSED, gpointer user_data)
{
  OeMediaBin *self = OE_MEDIA_BIN (user_data);

  if (self->import_func == NULL || !G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
    return FALSE;

  /* The file list and its GFiles are borrowed; copy the paths out. */
  GSList *files = gdk_file_list_get_files (g_value_get_boxed (value));
  GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);

  for (GSList *iter = files; iter != NULL; iter = iter->next)
    {
      const gchar *path = g_file_peek_path (G_FILE (iter->data));

      if (path != NULL)
        g_ptr_array_add (paths, g_strdup (path));
    }

  g_ptr_array_add (paths, NULL);
  self->import_func ((const gchar *const *) paths->pdata, self->import_data);
  g_ptr_array_unref (paths);
  return TRUE;
}

static void
on_drop_enter (GtkDropTarget *target G_GNUC_UNUSED, gdouble x G_GNUC_UNUSED,
               gdouble y G_GNUC_UNUSED, gpointer user_data)
{
  gtk_widget_add_css_class (GTK_WIDGET (user_data), "drop-active");
}

static void
on_drop_leave (GtkDropTarget *target G_GNUC_UNUSED, gpointer user_data)
{
  gtk_widget_remove_css_class (GTK_WIDGET (user_data), "drop-active");
}

/* ------------------------------------------------------------------ */
/* Type plumbing.                                                      */
/* ------------------------------------------------------------------ */

static void
oe_media_bin_dispose (GObject *object)
{
  OeMediaBin *self = OE_MEDIA_BIN (object);

  /* The import sink points back at the window; drop it first. */
  self->import_func = NULL;
  self->import_data = NULL;

  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))) != NULL)
    gtk_widget_unparent (child);

  G_OBJECT_CLASS (oe_media_bin_parent_class)->dispose (object);
}

static void
oe_media_bin_class_init (OeMediaBinClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = oe_media_bin_dispose;

  bin_signals[SIGNAL_SELECTION_CHANGED]
      = g_signal_new ("selection-changed", OE_TYPE_MEDIA_BIN, G_SIGNAL_RUN_FIRST, 0, NULL, NULL,
                      NULL, G_TYPE_NONE, 0);
  bin_signals[SIGNAL_RELINK_REQUESTED]
      = g_signal_new ("relink-requested", OE_TYPE_MEDIA_BIN, G_SIGNAL_RUN_FIRST, 0, NULL, NULL,
                      NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void
oe_media_bin_init (OeMediaBin *self)
{
  self->selected_id = 0;

  /* Panel chrome: titled frame like every other pane. */
  GtkWidget *panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (panel, "panel");

  GtkWidget *header = gtk_label_new ("Media Bin");

  gtk_widget_add_css_class (header, "panel-title");
  gtk_widget_set_halign (header, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (panel), header);

  self->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->stack, TRUE);

  GtkWidget *empty = gtk_label_new (
      "No media imported yet — use File ▸ Import Media… (Ctrl+I) or drop files here");

  gtk_label_set_wrap (GTK_LABEL (empty), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (empty), 24);
  gtk_widget_add_css_class (empty, "empty-state");
  gtk_widget_set_halign (empty, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (empty, GTK_ALIGN_CENTER);
  gtk_stack_add_named (GTK_STACK (self->stack), empty, "empty");

  GtkWidget *scroller = gtk_scrolled_window_new ();

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  self->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->list), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (self->list), TRUE);
  gtk_widget_add_css_class (self->list, "media-bin-list");
  g_signal_connect (self->list, "selected-rows-changed", G_CALLBACK (on_selection_changed), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), self->list);
  gtk_stack_add_named (GTK_STACK (self->stack), scroller, "list");

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
  gtk_box_append (GTK_BOX (panel), self->stack);

  gtk_widget_set_parent (panel, GTK_WIDGET (self));

  /* Drop target: file lists from the file manager, copy action only. */
  GtkDropTarget *target = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);

  g_signal_connect (target, "drop", G_CALLBACK (on_drop), self);
  g_signal_connect (target, "enter", G_CALLBACK (on_drop_enter), self);
  g_signal_connect (target, "leave", G_CALLBACK (on_drop_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (target));
}

OeMediaBin *
oe_media_bin_new (OeMediaLibrary *library)
{
  g_return_val_if_fail (library != NULL, NULL);

  OeMediaBin *bin = g_object_new (OE_TYPE_MEDIA_BIN, NULL);

  bin->library = library;
  return bin;
}

void
oe_media_bin_set_import_func (OeMediaBin *bin, OeMediaBinImportFunc func, gpointer user_data)
{
  g_return_if_fail (OE_IS_MEDIA_BIN (bin));

  bin->import_func = func;
  bin->import_data = user_data;
}

void
oe_media_bin_set_library (OeMediaBin *bin, OeMediaLibrary *library)
{
  g_return_if_fail (OE_IS_MEDIA_BIN (bin));
  g_return_if_fail (library != NULL);

  /* Session replacement: the bin still owns nothing — it just points at
   * whichever library the window currently keeps alive. The next refresh
   * rebuilds every row from the new store. */
  bin->library = library;
}
