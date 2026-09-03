/* oe_theme.c — theme loader implementation (Phase 1).
 *
 * The stylesheet ships inside a GResource compiled into the binary, so a
 * mis-installed data file can never leave the app unstyled. GTK only: this
 * file is excluded from the GTK-free unit tests.
 */

#include "oe_theme.h"

#include <gtk/gtk.h>

#include "../app/oe_log.h"

#define OE_THEME_RESOURCE "/com/mempickup/obvious-edit/ui/obvious-edit.css"

static gboolean theme_loaded = FALSE;

void
oe_theme_init (void)
{
  GdkDisplay *display;

  if (theme_loaded)
    return;

  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, OE_THEME_RESOURCE);

  display = gdk_display_get_default ();
  if (display == NULL)
    {
      /* Only reachable before gtk_init(); the application calls this after
       * startup, so treat as a programming error, not a runtime one. */
      g_error ("oe_theme_init called before a display exists");
    }

  gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);

  theme_loaded = TRUE;
  oe_log (OE_LOG_LEVEL_INFO, "theme loaded from resource");
}
