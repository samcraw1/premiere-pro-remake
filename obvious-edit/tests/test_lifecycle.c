/* test_lifecycle.c — Phase 0 smoke tests for the lifecycle adapters.
 *
 * Three GLib test cases, all display-free:
 *   /lifecycle/ffmpeg        init is idempotent, shutdown is paired and safe.
 *   /lifecycle/audio-output  SDL3 audio init/quit round-trips twice.
 *   /log/level-override      OE_LOG_LEVEL selects the emission threshold.
 */

#include <glib.h>

#include "../src/app/oe_log.h"
#include "../src/media/oe_ffmpeg.h"
#include "../src/playback/oe_audio_output.h"

/*
 * Capturing log writer: installed once for the whole binary, it records the
 * MESSAGE field of every record in the OE domain so tests can assert what
 * oe_log emitted. Non-OE records are returned UNHANDLED so GLib falls back
 * to the default writer and framework diagnostics stay visible.
 */
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

static void
captured_reset (void)
{
  g_string_truncate (captured, 0);
}

static gboolean
captured_contains (const gchar *needle)
{
  return strstr (captured->str, needle) != NULL;
}

static void
test_ffmpeg_lifecycle (void)
{
  GError *error = NULL;

  g_assert_false (oe_ffmpeg_is_initialized ());

  g_assert_true (oe_ffmpeg_init (&error));
  g_assert_no_error (error);
  g_assert_true (oe_ffmpeg_is_initialized ());

  /* A second init is an idempotent no-op and must not touch the error slot. */
  g_assert_true (oe_ffmpeg_init (&error));
  g_assert_no_error (error);
  g_assert_true (oe_ffmpeg_is_initialized ());

  oe_ffmpeg_shutdown ();
  g_assert_false (oe_ffmpeg_is_initialized ());

  /* Double shutdown, and shutdown before init, are both safe. */
  oe_ffmpeg_shutdown ();
  oe_ffmpeg_shutdown ();
  g_assert_false (oe_ffmpeg_is_initialized ());

  /* Re-initialisation after shutdown works. */
  g_assert_true (oe_ffmpeg_init (&error));
  g_assert_no_error (error);
  g_assert_true (oe_ffmpeg_is_initialized ());
  oe_ffmpeg_shutdown ();
  g_assert_false (oe_ffmpeg_is_initialized ());
}

static void
test_audio_output_lifecycle (void)
{
  GError *error = NULL;

  g_assert_false (oe_audio_output_is_initialized ());

  g_assert_true (oe_audio_output_init (&error));
  g_assert_no_error (error);
  g_assert_true (oe_audio_output_is_initialized ());

  g_assert_true (oe_audio_output_init (&error));
  g_assert_no_error (error);
  g_assert_true (oe_audio_output_is_initialized ());

  oe_audio_output_shutdown ();
  g_assert_false (oe_audio_output_is_initialized ());

  oe_audio_output_shutdown ();
  oe_audio_output_shutdown ();
  g_assert_false (oe_audio_output_is_initialized ());

  /* Full round-trip a second time to catch one-time-only teardown bugs. */
  g_assert_true (oe_audio_output_init (&error));
  g_assert_no_error (error);
  g_assert_true (oe_audio_output_is_initialized ());
  oe_audio_output_shutdown ();
  g_assert_false (oe_audio_output_is_initialized ());
}

static void
test_log_level_override (void)
{
  g_setenv ("OE_LOG_LEVEL", "debug", TRUE);
  oe_log_init ();
  g_assert_cmpint (oe_log_get_level (), ==, OE_LOG_LEVEL_DEBUG);

  captured_reset ();
  oe_log (OE_LOG_LEVEL_DEBUG, "override-probe-debug");
  g_assert_true (captured_contains ("override-probe-debug"));

  /* Raising the threshold hides debug records but keeps errors flowing. */
  g_setenv ("OE_LOG_LEVEL", "error", TRUE);
  oe_log_init ();
  g_assert_cmpint (oe_log_get_level (), ==, OE_LOG_LEVEL_ERROR);

  captured_reset ();
  oe_log (OE_LOG_LEVEL_DEBUG, "override-probe-suppressed");
  g_assert_false (captured_contains ("override-probe-suppressed"));

  captured_reset ();
  oe_log (OE_LOG_LEVEL_ERROR, "override-probe-error");
  g_assert_true (captured_contains ("override-probe-error"));

  g_unsetenv ("OE_LOG_LEVEL");
  oe_log_init ();
  g_assert_cmpint (oe_log_get_level (), ==, OE_LOG_LEVEL_INFO);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  captured = g_string_new ("");
  g_log_set_writer_func (capture_writer, NULL, NULL);

  g_test_add_func ("/lifecycle/ffmpeg", test_ffmpeg_lifecycle);
  g_test_add_func ("/lifecycle/audio-output", test_audio_output_lifecycle);
  g_test_add_func ("/log/level-override", test_log_level_override);

  return g_test_run ();
}
