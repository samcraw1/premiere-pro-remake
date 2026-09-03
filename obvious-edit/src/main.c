/* main.c — Obvious Edit entry point (Phase 0).
 *
 * Owns logging setup and the application run loop. Lifecycle adapters run in
 * OeApplication's startup and shutdown vfuncs, so --self-check exercises the
 * same startup and shutdown paths as a normal session.
 */

#include <glib.h>
#include <gtk/gtk.h>

#include "app/oe_application.h"
#include "app/oe_log.h"

int
main (int argc, char **argv)
{
  OeApplication *application;
  int exit_code;

  oe_log_init ();

  application = oe_application_new ();

  exit_code = g_application_run (G_APPLICATION (application), argc, argv);

  if (oe_application_startup_attempted (application)
      && !oe_application_startup_succeeded (application))
    exit_code = 1;

  g_object_unref (application);
  return exit_code;
}
