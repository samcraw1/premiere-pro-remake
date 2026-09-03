#include "oe_log.h"

/*
 * The GLib default writer gates INFO and DEBUG records on G_MESSAGES_DEBUG.
 * oe_log_init() forwards the OE domain into that variable when the requested
 * threshold is INFO or lower, so OE_LOG_LEVEL alone controls visibility.
 * See docs/learning/phase-0.md, "Structured logging", for the walkthrough.
 */

static OeLogLevel oe_log_threshold = OE_LOG_LEVEL_INFO;

static const gchar *
oe_log_level_name (OeLogLevel level)
{
  switch (level)
    {
    case OE_LOG_LEVEL_DEBUG:
      return "debug";
    case OE_LOG_LEVEL_INFO:
      return "info";
    case OE_LOG_LEVEL_WARNING:
      return "warning";
    case OE_LOG_LEVEL_ERROR:
      return "error";
    }

  return "unknown";
}

static GLogLevelFlags
oe_log_level_to_glib (OeLogLevel level)
{
  switch (level)
    {
    case OE_LOG_LEVEL_DEBUG:
      return G_LOG_LEVEL_DEBUG;
    case OE_LOG_LEVEL_INFO:
      return G_LOG_LEVEL_INFO;
    case OE_LOG_LEVEL_WARNING:
      return G_LOG_LEVEL_WARNING;
    case OE_LOG_LEVEL_ERROR:
      return G_LOG_LEVEL_CRITICAL;
    }

  return G_LOG_LEVEL_WARNING;
}

/* Syslog-style priority value carried as the structured PRIORITY field. */
static const gchar *
oe_log_level_priority (OeLogLevel level)
{
  switch (level)
    {
    case OE_LOG_LEVEL_DEBUG:
      return "7";
    case OE_LOG_LEVEL_INFO:
      return "6";
    case OE_LOG_LEVEL_WARNING:
      return "4";
    case OE_LOG_LEVEL_ERROR:
      return "3";
    }

  return "6";
}

/* Emits unconditionally; used before a threshold is known. */
static void
oe_log_emit (OeLogLevel level, const gchar *message)
{
  GLogField fields[] = {
    { "PRIORITY", oe_log_level_priority (level), -1 },
    { "GLIB_DOMAIN", G_LOG_DOMAIN, -1 },
    { "OE_LEVEL", oe_log_level_name (level), -1 },
    { "MESSAGE", message, -1 },
  };

  g_log_structured_array (oe_log_level_to_glib (level), fields, G_N_ELEMENTS (fields));
}

/* Returns FALSE through @known when @value is not a level name. */
static OeLogLevel
oe_log_level_parse (const gchar *value, gboolean *known)
{
  g_autofree gchar *lower = NULL;

  *known = TRUE;

  if (value == NULL || *value == '\0')
    return OE_LOG_LEVEL_INFO;

  lower = g_ascii_strdown (value, -1);

  if (g_strcmp0 (lower, "debug") == 0)
    return OE_LOG_LEVEL_DEBUG;
  if (g_strcmp0 (lower, "info") == 0)
    return OE_LOG_LEVEL_INFO;
  if (g_strcmp0 (lower, "warning") == 0)
    return OE_LOG_LEVEL_WARNING;
  if (g_strcmp0 (lower, "error") == 0)
    return OE_LOG_LEVEL_ERROR;

  *known = FALSE;
  return OE_LOG_LEVEL_INFO;
}

/*
 * GLib's default writer compares the domains listed in G_MESSAGES_DEBUG
 * (comma-separated, or the special value "all") against the GLIB_DOMAIN
 * field of each record; INFO and DEBUG records whose domain is absent are
 * dropped. Append the OE domain when it is missing so the threshold selected
 * through OE_LOG_LEVEL actually governs what appears on screen.
 */
static void
ensure_debug_domains_forwarded (void)
{
  const gchar *current = g_getenv ("G_MESSAGES_DEBUG");
  gboolean present;
  gchar **domains;

  if (current == NULL)
    {
      g_setenv ("G_MESSAGES_DEBUG", G_LOG_DOMAIN, TRUE);
      return;
    }

  if (g_strcmp0 (current, "all") == 0)
    return;

  domains = g_strsplit (current, ",", -1);
  present = g_strv_contains ((const gchar *const *) domains, G_LOG_DOMAIN);
  g_strfreev (domains);

  if (!present)
    {
      g_autofree gchar *joined = g_strconcat (current, ",", G_LOG_DOMAIN, NULL);

      g_setenv ("G_MESSAGES_DEBUG", joined, TRUE);
    }
}

void
oe_log_init (void)
{
  gboolean known = TRUE;

  oe_log_threshold = oe_log_level_parse (g_getenv ("OE_LOG_LEVEL"), &known);

  if (!known)
    {
      oe_log_emit (OE_LOG_LEVEL_WARNING, "unknown OE_LOG_LEVEL value; using info");
    }

  if (oe_log_threshold <= OE_LOG_LEVEL_INFO)
    ensure_debug_domains_forwarded ();
}

void
oe_log (OeLogLevel level, const gchar *format, ...)
{
  va_list args;
  gchar *message;

  if (level < oe_log_threshold)
    return;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  oe_log_emit (level, message);
  g_free (message);
}

OeLogLevel
oe_log_get_level (void)
{
  return oe_log_threshold;
}
