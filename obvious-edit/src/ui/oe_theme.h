/* oe_theme.h — the shell's original dark theme (Phase 1). */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * oe_theme_init:
 *
 * Loads the compiled-in CSS resource into a GtkCssProvider and applies it
 * to the default display. Idempotent: repeated calls are no-ops. Loads
 * once at application startup, before the first window is realized.
 */
void oe_theme_init (void);

G_END_DECLS
