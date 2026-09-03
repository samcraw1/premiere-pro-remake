/* oe_application.h — the GtkApplication shell (Phase 0).
 *
 * Owns startup/shutdown ordering for the lifecycle adapters and the
 * --self-check mode used by scripts/run-headless.sh.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define OE_TYPE_APPLICATION (oe_application_get_type ())

G_DECLARE_FINAL_TYPE (OeApplication, oe_application, OE, APPLICATION, GtkApplication)

/**
 * oe_application_new:
 *
 * Returns: (transfer full): a new OeApplication with the Obvious Edit
 * application id and the --self-check option registered. Caller frees with
 * g_object_unref().
 */
OeApplication *oe_application_new (void);

/**
 * oe_application_set_self_check:
 * @self: the application
 * @self_check: whether the next run quits after the window is first mapped
 *
 * Must be called before g_application_run().
 */
void oe_application_set_self_check (OeApplication *self, gboolean self_check);

/**
 * oe_application_startup_attempted:
 *
 * Returns: TRUE once the startup vfunc has run at least once.
 */
gboolean oe_application_startup_attempted (OeApplication *self);

/**
 * oe_application_startup_succeeded:
 *
 * Returns: TRUE when the lifecycle adapters started without error. Only
 * meaningful after oe_application_startup_attempted() returns TRUE.
 */
gboolean oe_application_startup_succeeded (OeApplication *self);

G_END_DECLS
