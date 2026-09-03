#include "oe_main_window.h"

#include "../app/oe_log.h"

struct _OeMainWindow
{
  GtkApplicationWindow parent_instance;
};

G_DEFINE_TYPE (OeMainWindow, oe_main_window, GTK_TYPE_APPLICATION_WINDOW)

static void
oe_main_window_init (OeMainWindow *self)
{
  gtk_window_set_title (GTK_WINDOW (self), "Obvious Edit");
  gtk_window_set_default_size (GTK_WINDOW (self), 1280, 720);
}

static void
oe_main_window_class_init (OeMainWindowClass *klass G_GNUC_UNUSED)
{
  /* Phase 0: no signals, properties, or children yet. */
}

GtkWidget *
oe_main_window_new (GtkApplication *application)
{
  g_return_val_if_fail (GTK_IS_APPLICATION (application), NULL);

  oe_log (OE_LOG_LEVEL_DEBUG, "main window created");
  return GTK_WIDGET (g_object_new (OE_TYPE_MAIN_WINDOW, "application", application, NULL));
}
