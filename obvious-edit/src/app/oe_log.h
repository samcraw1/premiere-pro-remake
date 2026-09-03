/* oe_log.h — Obvious Edit structured logging (Phase 0).
 *
 * Thin wrapper over GLib structured logging with a single "oe" log domain.
 * Every diagnostic the project emits goes through oe_log(); stdio output is
 * reserved for library internals we do not control.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * OeLogLevel:
 * @OE_LOG_LEVEL_DEBUG: high-volume detail, hidden unless requested.
 * @OE_LOG_LEVEL_INFO: lifecycle milestones and version reports.
 * @OE_LOG_LEVEL_WARNING: recoverable surprises.
 * @OE_LOG_LEVEL_ERROR: handled failures (init errors); not process-fatal.
 *
 * Ordered by severity, so thresholds compare with < and >.
 *
 * Mapping to GLib levels: DEBUG and INFO map to themselves, WARNING to
 * G_LOG_LEVEL_WARNING, and ERROR to G_LOG_LEVEL_CRITICAL, because GLib treats
 * G_LOG_LEVEL_ERROR as abort-the-process — handled errors that return a
 * #GError must not abort.
 */
typedef enum
{
  OE_LOG_LEVEL_DEBUG,
  OE_LOG_LEVEL_INFO,
  OE_LOG_LEVEL_WARNING,
  OE_LOG_LEVEL_ERROR,
} OeLogLevel;

/**
 * oe_log_init:
 *
 * Reads the OE_LOG_LEVEL environment variable (debug, info, warning, error;
 * case-insensitive; default: info) and stores it as the emission threshold.
 * May be called again to re-read the variable — the smoke tests rely on that.
 *
 * When the threshold allows INFO or DEBUG, the OE domain is forwarded into
 * G_MESSAGES_DEBUG, because GLib's default log writer drops INFO and DEBUG
 * records unless their domain is listed there. Without the forwarding,
 * OE_LOG_LEVEL=info would parse correctly yet print nothing.
 */
void oe_log_init (void);

/**
 * oe_log:
 * @level: severity of the record
 * @format: printf-style format string
 * @...: format arguments
 *
 * Emits one structured record in the OE log domain. Records below the
 * threshold selected by oe_log_init() are dropped before they reach GLib.
 */
void oe_log (OeLogLevel level, const gchar *format, ...) G_GNUC_PRINTF (2, 3);

/**
 * oe_log_get_level:
 *
 * Returns: the threshold most recently installed by oe_log_init().
 */
OeLogLevel oe_log_get_level (void);

G_END_DECLS
