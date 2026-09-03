/* oe_shell_layout.c — layout persistence implementation (Phase 1).
 *
 * A GKeyFile in the "layout" group carries the version key plus the panel
 * splitter positions. Save is atomic via g_file_set_contents() (temp file
 * + rename in the same directory). Load follows the header's rules and
 * never fails the launch: every recoverable problem falls back to safe
 * data with a log line instead of an error return.
 */

#include "oe_shell_layout.h"

#include <stdlib.h>

#include "../app/oe_log.h"

#define LAYOUT_GROUP "layout"

void
oe_shell_layout_defaults (OeShellLayout *l)
{
  g_return_if_fail (l != NULL);

  *l = (OeShellLayout) {
    .version = OE_SHELL_LAYOUT_VERSION,
    .window_width = 1280,
    .window_height = 720,
    .window_maximized = FALSE,
    .bin_width = 280,
    .inspector_width = 320,
    .timeline_height = 380,
  };
}

gchar *
oe_shell_layout_default_path (void)
{
  /* XDG_CONFIG_HOME is read directly instead of via g_get_user_config_dir():
   * GLib caches the XDG dirs per process, while evidence runs need an env
   * override to take effect. Unset/empty falls back to $HOME/.config, as in
   * the XDG Basedir spec. */
  const gchar *xdg = g_getenv ("XDG_CONFIG_HOME");

  if (xdg != NULL && *xdg != '\0')
    return g_build_filename (xdg, "obvious-edit", "layout.conf", NULL);

  return g_build_filename (g_get_home_dir (), ".config", "obvious-edit", "layout.conf", NULL);
}

static gboolean
save_layout (const OeShellLayout *l, const gchar *path, GError **error)
{
  g_return_val_if_fail (l != NULL, FALSE);
  g_return_val_if_fail (path != NULL && *path != '\0', FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  GKeyFile *key_file = g_key_file_new ();

  g_key_file_set_int64 (key_file, LAYOUT_GROUP, "version", l->version);
  g_key_file_set_int64 (key_file, LAYOUT_GROUP, "window-width", l->window_width);
  g_key_file_set_int64 (key_file, LAYOUT_GROUP, "window-height", l->window_height);
  g_key_file_set_boolean (key_file, LAYOUT_GROUP, "window-maximized", l->window_maximized);
  g_key_file_set_int64 (key_file, LAYOUT_GROUP, "bin-width", l->bin_width);
  g_key_file_set_int64 (key_file, LAYOUT_GROUP, "inspector-width", l->inspector_width);
  g_key_file_set_int64 (key_file, LAYOUT_GROUP, "timeline-height", l->timeline_height);

  gchar *data = g_key_file_to_data (key_file, NULL, NULL);
  gchar *directory = g_path_get_dirname (path);

  /* A missing config directory is created; an unwritable one surfaces as a
   * write failure from g_file_set_contents below. */
  g_mkdir_with_parents (directory, 0755);

  gboolean ok = g_file_set_contents (path, data, -1, error);

  if (!ok && error != NULL && *error == NULL)
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "could not write %s", path);

  g_free (directory);
  g_free (data);
  g_key_file_unref (key_file);

  if (ok)
    oe_log (OE_LOG_LEVEL_DEBUG, "layout saved to %s", path);
  else
    oe_log (OE_LOG_LEVEL_WARNING, "layout save to %s failed", path);

  return ok;
}

gboolean
oe_shell_layout_save (const OeShellLayout *l, GError **error)
{
  gchar *path = oe_shell_layout_default_path ();
  gboolean ok = save_layout (l, path, error);

  g_free (path);
  return ok;
}

gboolean
oe_shell_layout_save_to (const OeShellLayout *l, const gchar *path, GError **error)
{
  return save_layout (l, path, error);
}

/* One integer field: missing or unparseable -> default; below the minimum
 * -> clamped. */
static int
read_field (GKeyFile *key_file, const gchar *key, int default_value, int minimum)
{
  GError *error = NULL;
  gint64 value = g_key_file_get_int64 (key_file, LAYOUT_GROUP, key, &error);

  if (error != NULL)
    {
      g_error_free (error);
      return default_value;
    }

  if (value < minimum)
    return minimum;

  if (value > G_MAXINT32)
    return default_value;

  return (int) value;
}

static void
load_into (OeShellLayout *l, const gchar *path)
{
  GKeyFile *key_file = g_key_file_new ();
  GError *error = NULL;

  oe_shell_layout_defaults (l);

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      /* First launch: documented defaults, no warning. */
      oe_log (OE_LOG_LEVEL_INFO, "no layout file at %s; using defaults", path);
      g_key_file_unref (key_file);
      return;
    }

  if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, &error))
    {
      oe_log (OE_LOG_LEVEL_WARNING, "layout file %s unreadable (%s); using defaults", path,
              error->message);
      g_error_free (error);
      g_key_file_unref (key_file);
      return;
    }

  gint64 version = g_key_file_get_int64 (key_file, LAYOUT_GROUP, "version", &error);

  if (error != NULL)
    {
      g_error_free (error);
      oe_log (OE_LOG_LEVEL_WARNING, "layout file %s has no valid version; using defaults", path);
      g_key_file_unref (key_file);
      return;
    }

  if (version > OE_SHELL_LAYOUT_VERSION)
    {
      oe_log (OE_LOG_LEVEL_WARNING,
              "layout file version %lld is newer than supported %d; using defaults",
              (long long) version, OE_SHELL_LAYOUT_VERSION);
      g_key_file_unref (key_file);
      return;
    }

  if (version < OE_SHELL_LAYOUT_VERSION)
    {
      oe_log (OE_LOG_LEVEL_WARNING, "layout file version %lld is invalid; using defaults",
              (long long) version);
      g_key_file_unref (key_file);
      return;
    }

  l->version = (int) version;
  l->window_width = read_field (key_file, "window-width", 1280, OE_SHELL_LAYOUT_MIN_WIDTH);
  l->window_height = read_field (key_file, "window-height", 720, OE_SHELL_LAYOUT_MIN_HEIGHT);
  l->bin_width = read_field (key_file, "bin-width", 280, OE_SHELL_LAYOUT_MIN_BIN_WIDTH);
  l->inspector_width
      = read_field (key_file, "inspector-width", 320, OE_SHELL_LAYOUT_MIN_INSPECTOR_WIDTH);
  l->timeline_height
      = read_field (key_file, "timeline-height", 380, OE_SHELL_LAYOUT_MIN_TIMELINE_HEIGHT);

  GError *maximized_error = NULL;
  gboolean maximized
      = g_key_file_get_boolean (key_file, LAYOUT_GROUP, "window-maximized", &maximized_error);

  l->window_maximized = maximized_error == NULL && maximized;
  g_clear_error (&maximized_error);

  g_key_file_unref (key_file);

  oe_log (OE_LOG_LEVEL_INFO, "layout loaded from %s: %dx%d (bin %d, inspector %d, timeline %d)",
          path, l->window_width, l->window_height, l->bin_width, l->inspector_width,
          l->timeline_height);
}

gboolean
oe_shell_layout_load (OeShellLayout *l, GError **error G_GNUC_UNUSED)
{
  gchar *path = oe_shell_layout_default_path ();

  g_return_val_if_fail (l != NULL, FALSE);

  load_into (l, path);
  g_free (path);
  return TRUE;
}

gboolean
oe_shell_layout_load_from (OeShellLayout *l, const gchar *path, GError **error G_GNUC_UNUSED)
{
  g_return_val_if_fail (l != NULL, FALSE);
  g_return_val_if_fail (path != NULL && *path != '\0', FALSE);

  load_into (l, path);
  return TRUE;
}
