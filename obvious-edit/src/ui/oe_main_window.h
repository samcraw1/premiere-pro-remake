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

G_END_DECLS
