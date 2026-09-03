/* oe_main_window.c — the Phase 1 editor shell implementation.
 *
 * Composition rules for this file:
 *   - Layout is built from GtkPaned/GtkBox only. No GtkFixed, no absolute
 *     pixel placement: every panel is a splitter child and resizes with
 *     the window.
 *   - Every panel is labeled and shows an empty-state line that names the
 *     phase which will fill it.
 *   - The window is the seam between GTK and the GTK-free command
 *     registry: it installs the reporter that writes dispatch feedback to
 *     the status bar, and never calls registry internals.
 *   - Layout persistence flows through the OeShellLayout struct only:
 *     load at construction (defaults on first run), save on close-request.
 */

#include "oe_main_window.h"

#include <gtk/gtk.h>

#include "../app/oe_command.h"
#include "../app/oe_log.h"
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

  int pending_bin;
  int pending_inspector;
  int pending_timeline;
  gboolean positions_applied;
  gboolean layout_saved;
};

G_DEFINE_TYPE (OeMainWindow, oe_main_window, GTK_TYPE_APPLICATION_WINDOW)

/* ------------------------------------------------------------------ */
/* Command reporter: dispatch feedback lands in the status bar.        */
/* ------------------------------------------------------------------ */

static void
report_to_status_bar (OeCommandId id G_GNUC_UNUSED, const gchar *message, gpointer user_data)
{
  OeMainWindow *self = OE_MAIN_WINDOW (user_data);

  gtk_label_set_text (GTK_LABEL (self->status_label), message);
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
  gtk_box_append (GTK_BOX (bar), toolbar_button ("Import…", "media.import", "Import media"));
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
  /* The reporter points back at this window; clear it before the status
   * bar child is destroyed so a late dispatch can never touch a dangling
   * widget. */
  oe_command_set_reporter (NULL, NULL);

  G_OBJECT_CLASS (oe_main_window_parent_class)->dispose (object);
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
  gtk_paned_set_start_child (GTK_PANED (self->bin_paned),
                             panel_new ("Media Bin", "No media imported yet — Phase 2"));

  self->inspector_paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_shrink_start_child (GTK_PANED (self->inspector_paned), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (self->inspector_paned), FALSE);

  GtkWidget *monitors = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_box_set_homogeneous (GTK_BOX (monitors), TRUE);
  gtk_box_append (GTK_BOX (monitors), panel_new ("Source Monitor", "No clip loaded yet — Phase 2"));
  gtk_box_append (GTK_BOX (monitors),
                  panel_new ("Program Monitor", "No sequence loaded yet — Phase 3"));

  gtk_paned_set_start_child (GTK_PANED (self->inspector_paned), monitors);
  gtk_paned_set_end_child (GTK_PANED (self->inspector_paned),
                           panel_new ("Inspector", "No selection — properties appear here"));

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

  /* Dispatch feedback lands in the status bar until handlers exist. */
  oe_command_set_reporter (report_to_status_bar, self);

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
