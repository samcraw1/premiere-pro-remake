/* test_shell_layout.c — GTK-free tests for shell layout persistence (Phase 1).
 *
 * Seven GLib test cases, all display-free:
 *   /shell-layout/defaults      the documented default layout.
 *   /shell-layout/round-trip    save then load reproduces every field,
 *                               including a versioned file on disk.
 *   /shell-layout/missing       a missing file falls back to defaults and
 *                               never fails the launch.
 *   /shell-layout/newer-version a newer version group keeps defaults and
 *                               logs a warning.
 *   /shell-layout/corrupt-file  an unparseable file falls back to defaults.
 *   /shell-layout/clamping      corrupt fields clamp to sane minimums.
 *   /shell-layout/partial       missing keys keep their default values.
 *
 * Tests always use explicit paths (oe_shell_layout_save_to/load_from) so
 * they never touch the user's real configuration.
 */

#include <glib/gstdio.h>

#include <string.h>

#include "../src/app/oe_log.h"
#include "../src/ui/oe_shell_layout.h"

/* Capturing log writer, same pattern as test_lifecycle.c. */
static GString *captured = NULL;

static GLogWriterOutput
capture_writer (GLogLevelFlags log_level, const GLogField *fields, gsize n_fields,
                gpointer user_data G_GNUC_UNUSED)
{
  gboolean in_oe_domain = FALSE;
  gsize i;

  for (i = 0; i < n_fields; i++)
    {
      if (g_strcmp0 (fields[i].key, "GLIB_DOMAIN") == 0
          && g_strcmp0 ((const gchar *) fields[i].value, G_LOG_DOMAIN) == 0)
        {
          in_oe_domain = TRUE;
          break;
        }
    }

  if (!in_oe_domain)
    return G_LOG_WRITER_UNHANDLED;

  for (i = 0; i < n_fields; i++)
    {
      if (g_strcmp0 (fields[i].key, "MESSAGE") == 0)
        g_string_append_printf (captured, "[%d] %s\n", (int) log_level,
                                (const gchar *) fields[i].value);
    }

  return G_LOG_WRITER_HANDLED;
}

static gboolean
captured_contains (const gchar *needle)
{
  return strstr (captured->str, needle) != NULL;
}

/* Per-test scratch directory, removed with the fixture. */
typedef struct
{
  gchar *dir;
} TmpDir;

static void
tmp_dir_set_up (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  fx->dir = g_dir_make_tmp ("oe-shell-layout-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (fx->dir);
  g_string_truncate (captured, 0);
}

static void
tmp_dir_tear_down (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  if (fx->dir == NULL)
    return;

  const gchar *name;
  GDir *dir = g_dir_open (fx->dir, 0, NULL);

  if (dir != NULL)
    {
      while ((name = g_dir_read_name (dir)) != NULL)
        {
          gchar *path = g_build_filename (fx->dir, name, NULL);

          g_remove (path);
          g_free (path);
        }
      g_dir_close (dir);
    }
  g_rmdir (fx->dir);
  g_free (fx->dir);
  fx->dir = NULL;
}

static gchar *
tmp_path (TmpDir *fx, const gchar *file)
{
  return g_build_filename (fx->dir, file, NULL);
}

/* Writes raw file content (ini text or garbage) into the scratch dir. */
static gchar *
write_raw (TmpDir *fx, const gchar *file, const gchar *content)
{
  gchar *path = tmp_path (fx, file);

  g_assert_true (g_file_set_contents (path, content, -1, NULL));
  return path;
}

static void
assert_layout_equals (const OeShellLayout *a, const OeShellLayout *b)
{
  g_assert_cmpint (a->version, ==, b->version);
  g_assert_cmpint (a->window_width, ==, b->window_width);
  g_assert_cmpint (a->window_height, ==, b->window_height);
  g_assert_cmpint (a->window_maximized, ==, b->window_maximized);
  g_assert_cmpint (a->bin_width, ==, b->bin_width);
  g_assert_cmpint (a->inspector_width, ==, b->inspector_width);
  g_assert_cmpint (a->timeline_height, ==, b->timeline_height);
}

static void
test_defaults (void)
{
  OeShellLayout layout;

  oe_shell_layout_defaults (&layout);

  g_assert_cmpint (layout.version, ==, OE_SHELL_LAYOUT_VERSION);
  g_assert_cmpint (layout.window_width, ==, 1280);
  g_assert_cmpint (layout.window_height, ==, 720);
  g_assert_false (layout.window_maximized);
  g_assert_cmpint (layout.bin_width, ==, 280);
  g_assert_cmpint (layout.inspector_width, ==, 320);
  g_assert_cmpint (layout.timeline_height, ==, 380);
}

static void
test_round_trip (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *path = tmp_path (fx, "layout.conf");
  OeShellLayout saved;
  OeShellLayout loaded;
  GError *error = NULL;

  oe_shell_layout_defaults (&saved);
  saved.window_width = 1100;
  saved.window_height = 700;
  saved.window_maximized = TRUE;
  saved.bin_width = 250;
  saved.inspector_width = 300;
  saved.timeline_height = 350;

  g_assert_true (oe_shell_layout_save_to (&saved, path, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

  /* The file is a versioned key file: version key inside the layout group. */
  GKeyFile *raw = g_key_file_new ();

  g_assert_true (g_key_file_load_from_file (raw, path, G_KEY_FILE_NONE, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_key_file_get_integer (raw, "layout", "version", NULL), ==,
                   OE_SHELL_LAYOUT_VERSION);
  g_key_file_unref (raw);

  oe_shell_layout_defaults (&loaded);
  g_assert_true (oe_shell_layout_load_from (&loaded, path, &error));
  g_assert_no_error (error);
  assert_layout_equals (&saved, &loaded);

  /* Saving again to the same path replaces the file (atomic rename). */
  saved.window_width = 900;
  g_assert_true (oe_shell_layout_save_to (&saved, path, &error));
  g_assert_no_error (error);
  oe_shell_layout_defaults (&loaded);
  g_assert_true (oe_shell_layout_load_from (&loaded, path, &error));
  g_assert_no_error (error);
  assert_layout_equals (&saved, &loaded);

  g_free (path);
}

static void
test_missing_file (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *path = tmp_path (fx, "does-not-exist.conf");
  OeShellLayout layout;
  OeShellLayout defaults;
  GError *error = NULL;

  oe_shell_layout_defaults (&layout);
  layout.window_width = 1; /* garbage before load; load must replace it */

  oe_shell_layout_defaults (&defaults);

  g_assert_true (oe_shell_layout_load_from (&layout, path, &error));
  g_assert_no_error (error);
  assert_layout_equals (&layout, &defaults);

  /* A missing file is a first launch, not a corruption: no warning. */
  g_assert_false (captured_contains ("WARNING"));

  g_free (path);
}

static void
test_newer_version (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *path = write_raw (fx, "layout.conf",
                           "[layout]\n"
                           "version=99\n"
                           "window-width=800\n"
                           "window-height=600\n"
                           "bin-width=200\n"
                           "inspector-width=200\n"
                           "timeline-height=200\n");
  OeShellLayout layout;
  OeShellLayout defaults;
  GError *error = NULL;

  oe_shell_layout_defaults (&defaults);

  g_assert_true (oe_shell_layout_load_from (&layout, path, &error));
  g_assert_no_error (error);
  assert_layout_equals (&layout, &defaults);
  g_assert_true (captured_contains ("newer than supported"));

  g_free (path);
}

static void
test_corrupt_file (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *path = write_raw (fx, "layout.conf", "this is not a key file {{{\n\x01\x02");
  OeShellLayout layout;
  OeShellLayout defaults;
  GError *error = NULL;

  oe_shell_layout_defaults (&defaults);

  g_assert_true (oe_shell_layout_load_from (&layout, path, &error));
  g_assert_no_error (error);
  assert_layout_equals (&layout, &defaults);
  g_assert_true (captured_contains ("using defaults"));

  g_free (path);
}

static void
test_clamping (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *path = write_raw (fx, "layout.conf",
                           "[layout]\n"
                           "version=1\n"
                           "window-width=-10\n"
                           "window-height=0\n"
                           "window-maximized=false\n"
                           "bin-width=-5\n"
                           "inspector-width=0\n"
                           "timeline-height=3\n");
  OeShellLayout layout;
  GError *error = NULL;

  g_assert_true (oe_shell_layout_load_from (&layout, path, &error));
  g_assert_no_error (error);

  g_assert_cmpint (layout.version, ==, OE_SHELL_LAYOUT_VERSION);
  g_assert_cmpint (layout.window_width, ==, OE_SHELL_LAYOUT_MIN_WIDTH);
  g_assert_cmpint (layout.window_height, ==, OE_SHELL_LAYOUT_MIN_HEIGHT);
  g_assert_cmpint (layout.bin_width, ==, OE_SHELL_LAYOUT_MIN_BIN_WIDTH);
  g_assert_cmpint (layout.inspector_width, ==, OE_SHELL_LAYOUT_MIN_INSPECTOR_WIDTH);
  g_assert_cmpint (layout.timeline_height, ==, OE_SHELL_LAYOUT_MIN_TIMELINE_HEIGHT);

  g_free (path);
}

static void
test_partial_fields (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *path = write_raw (fx, "layout.conf",
                           "[layout]\n"
                           "version=1\n"
                           "window-width=1000\n");
  OeShellLayout layout;
  OeShellLayout defaults;
  GError *error = NULL;

  oe_shell_layout_defaults (&defaults);

  g_assert_true (oe_shell_layout_load_from (&layout, path, &error));
  g_assert_no_error (error);

  g_assert_cmpint (layout.window_width, ==, 1000);
  g_assert_cmpint (layout.window_height, ==, defaults.window_height);
  g_assert_cmpint (layout.bin_width, ==, defaults.bin_width);
  g_assert_cmpint (layout.inspector_width, ==, defaults.inspector_width);
  g_assert_cmpint (layout.timeline_height, ==, defaults.timeline_height);

  g_free (path);
}

static void
test_default_path (void)
{
  gchar *path = oe_shell_layout_default_path ();

  g_assert_nonnull (path);
  g_assert_true (g_str_has_suffix (path, "layout.conf"));
  g_assert_nonnull (strstr (path, "obvious-edit"));
  g_free (path);

  /* XDG_CONFIG_HOME is honoured so evidence runs can isolate state. */
  gchar *old = g_strdup (g_getenv ("XDG_CONFIG_HOME"));
  gchar *sandbox = g_dir_make_tmp ("oe-xdg-XXXXXX", NULL);

  g_assert_nonnull (sandbox);
  g_setenv ("XDG_CONFIG_HOME", sandbox, TRUE);

  path = oe_shell_layout_default_path ();
  g_assert_true (g_str_has_prefix (path, sandbox));
  g_free (path);

  g_unsetenv ("XDG_CONFIG_HOME");
  path = oe_shell_layout_default_path ();
  g_assert_nonnull (strstr (path, ".config"));
  g_free (path);

  if (old != NULL)
    g_setenv ("XDG_CONFIG_HOME", old, TRUE);
  else
    g_unsetenv ("XDG_CONFIG_HOME");
  g_free (old);
  g_rmdir (sandbox);
  g_free (sandbox);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  captured = g_string_new ("");
  g_log_set_writer_func (capture_writer, NULL, NULL);
  oe_log_init ();

  g_test_add_func ("/shell-layout/defaults", test_defaults);
  g_test_add_func ("/shell-layout/default-path", test_default_path);
  g_test_add ("/shell-layout/round-trip", TmpDir, NULL, tmp_dir_set_up, test_round_trip,
              tmp_dir_tear_down);
  g_test_add ("/shell-layout/missing", TmpDir, NULL, tmp_dir_set_up, test_missing_file,
              tmp_dir_tear_down);
  g_test_add ("/shell-layout/newer-version", TmpDir, NULL, tmp_dir_set_up, test_newer_version,
              tmp_dir_tear_down);
  g_test_add ("/shell-layout/corrupt-file", TmpDir, NULL, tmp_dir_set_up, test_corrupt_file,
              tmp_dir_tear_down);
  g_test_add ("/shell-layout/clamping", TmpDir, NULL, tmp_dir_set_up, test_clamping,
              tmp_dir_tear_down);
  g_test_add ("/shell-layout/partial", TmpDir, NULL, tmp_dir_set_up, test_partial_fields,
              tmp_dir_tear_down);

  return g_test_run ();
}
