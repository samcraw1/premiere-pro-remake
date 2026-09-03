/* oe_main_window.h — the Phase 0 application window.
 *
 * A titled, empty GtkApplicationWindow shell. Timeline and preview content
 * arrive in later phases; the title is the headless acceptance marker.
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
