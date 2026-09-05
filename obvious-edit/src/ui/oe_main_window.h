/* oe_main_window.h — the Phase 1 editor shell window.
 *
 * The shell composes the whole workspace from GtkPaned/GtkGrid containers:
 * a menu bar, a toolbar, the media bin, source and program monitors, the
 * inspector, the timeline area with transport controls, and a status bar.
 * Every panel shows a labeled empty state — the shell is never blank.
 *
 * The window owns the command reporter (status-bar feedback) and the
 * layout lifecycle: sizes and splitter positions load at construction and
 * save on close via oe_shell_layout.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define OE_TYPE_MAIN_WINDOW (oe_main_window_get_type ())

G_DECLARE_FINAL_TYPE (OeMainWindow, oe_main_window, OE, MAIN_WINDOW, GtkApplicationWindow)

/**
 * oe_main_window_new:
 * @application: the owning application
 *
 * Returns: (transfer none): a new OeMainWindow associated with @application.
 * The application owns the window; freeing the application destroys it.
 */
GtkWidget *oe_main_window_new (GtkApplication *application);

/**
 * oe_main_window_import_files:
 * @window: the editor shell
 * @paths: a %NULL-terminated array of filesystem paths
 *
 * Feeds paths through the same import pipeline the chooser and
 * drag-and-drop use. This is the headless smoke-test seam: --import-media
 * reaches it without touching the file chooser, which cannot be driven
 * under bare Xvfb.
 */
void oe_main_window_import_files (OeMainWindow *window, const gchar *const *paths);

/**
 * oe_main_window_import_and_insert_files:
 * @window: the editor shell
 * @paths: a %NULL-terminated array of filesystem paths
 *
 * The headless playback-dogfood seam: feeds paths through the import
 * pipeline and inserts every OK verdict on the first kind-matching
 * track at the playhead. Exists because a bare Xvfb session cannot
 * click a bin row any more than it can drive the file chooser.
 */
void oe_main_window_import_and_insert_files (OeMainWindow *window, const gchar *const *paths);

/**
 * oe_main_window_open_project_file:
 * @window: the editor shell
 * @path: a project file path
 *
 * Loads @path through the same flow the file chooser uses (strict
 * parse, session replacement, status-bar feedback). The headless
 * dogfood seam for project files: --open reaches it without the
 * chooser, which cannot be driven under bare Xvfb.
 */
void oe_main_window_open_project_file (OeMainWindow *window, const gchar *path);

/**
 * oe_main_window_get_snapping:
 * @window: the editor shell
 *
 * The timeline's session snapping flag (Phase 7). The application's
 * stateful edit.snap-toggle action mirrors this after every dispatch
 * so the Edit-menu check reflects the widget, not the action's own
 * history.
 */
gboolean oe_main_window_get_snapping (OeMainWindow *window);

G_END_DECLS
