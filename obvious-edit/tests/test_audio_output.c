/* test_audio_output.c — the SDL3 push-model stream adapter contract.
 *
 * Runs under SDL_AUDIODRIVER=dummy (set in the meson test environment),
 * so the default playback device is SDL's dummy: open succeeds without
 * hardware, is_dummy reports TRUE, and queue-depth accounting is fully
 * deterministic (the dummy device never consumes).
 *
 * The contract verified here, per the adapter header:
 *   - init is idempotent; shutdown is safe twice and before init
 *   - open before init is a typed error (NOT_INITIALIZED), never a crash
 *   - open reports a device rate, channel count, and the dummy flag
 *   - queue() accepts frames and queued_frames() accounts them
 *   - flush() drops everything; pause/resume preserve the queue
 *   - every entry point tolerates NULL streams
 *   - the stream is closed BEFORE oe_audio_output_shutdown()
 */

#include <glib.h>

#include "../src/playback/oe_audio_output.h"

/* g_test shuffles test order, so nothing may depend on registration
 * order: every stream-opening test ensures init itself (idempotent),
 * and the requires-init test restores the initialized state on exit. */
static void
ensure_audio_init (void)
{
  GError *error = NULL;

  g_assert_true (oe_audio_output_init (&error));
  g_assert_no_error (error);
  g_assert_true (oe_audio_output_is_initialized ());
}

/* 100 ms of stereo silence at any device rate, allocated per test. */
static float *
silence_frames (int sample_rate, int channels, gsize *out_frames)
{
  const gsize frames = (gsize) sample_rate / 10;

  *out_frames = frames;
  return g_new0 (float, frames *(gsize) channels);
}

static void
test_open_requires_init (void)
{
  /* Self-contained: force the uninitialized state, verify the typed
   * error, then restore init for the other tests in the shuffle. */
  oe_audio_output_shutdown ();
  g_assert_false (oe_audio_output_is_initialized ());

  OeAudioDeviceInfo info;
  GError *error = NULL;

  g_assert_null (oe_audio_output_open_stream (&info, &error));
  g_assert_nonnull (error);
  g_assert_error (error, OE_AUDIO_OUTPUT_ERROR, OE_AUDIO_OUTPUT_ERROR_NOT_INITIALIZED);
  g_error_free (error);

  ensure_audio_init ();
}

static void
test_open_reports_device_and_dummy (void)
{
  ensure_audio_init ();

  GError *error = NULL;
  OeAudioDeviceInfo info;

  OeAudioStream *stream = oe_audio_output_open_stream (&info, &error);

  if (error != NULL)
    g_test_message ("open failed: %s", error->message);
  g_assert_nonnull (stream);
  g_assert_no_error (error);
  g_assert_cmpint (info.sample_rate, >, 0);
  g_assert_cmpint (info.channels, >, 0);
  g_assert_true (info.is_dummy); /* SDL_AUDIODRIVER=dummy in the test env */

  oe_audio_output_close_stream (stream);
}

static void
test_queue_depth_and_flush (void)
{
  ensure_audio_init ();

  GError *error = NULL;
  OeAudioDeviceInfo info;
  OeAudioStream *stream = oe_audio_output_open_stream (&info, &error);

  g_assert_nonnull (stream);
  g_assert_cmpuint (oe_audio_output_queued_frames (stream), ==, 0);

  gsize n_frames = 0;
  float *frames = silence_frames (info.sample_rate, info.channels, &n_frames);

  /* The dummy device never consumes: the full push stays accounted. */
  g_assert_cmpuint (oe_audio_output_queue (stream, frames, n_frames), ==, n_frames);
  g_assert_cmpuint (oe_audio_output_queued_frames (stream), ==, n_frames);

  /* Flush drops everything (the seek discipline's first step). */
  oe_audio_output_flush (stream);
  g_assert_cmpuint (oe_audio_output_queued_frames (stream), ==, 0);

  /* Re-queue after flush: the stream stays usable. */
  g_assert_cmpuint (oe_audio_output_queue (stream, frames, n_frames), ==, n_frames);
  g_assert_cmpuint (oe_audio_output_queued_frames (stream), ==, n_frames);

  oe_audio_output_close_stream (stream);
  g_free (frames);
}

static void
test_pause_resume_preserves_queue (void)
{
  ensure_audio_init ();

  GError *error = NULL;
  OeAudioDeviceInfo info;
  OeAudioStream *stream = oe_audio_output_open_stream (&info, &error);

  g_assert_nonnull (stream);

  gsize n_frames = 0;
  float *frames = silence_frames (info.sample_rate, info.channels, &n_frames);

  g_assert_cmpuint (oe_audio_output_queue (stream, frames, n_frames), ==, n_frames);

  /* Pause, then resume: queued audio survives the round trip (and the
   * dummy device still has not consumed any of it). */
  oe_audio_output_set_running (stream, FALSE);
  g_assert_cmpuint (oe_audio_output_queued_frames (stream), ==, n_frames);

  oe_audio_output_set_running (stream, TRUE);
  g_assert_cmpuint (oe_audio_output_queued_frames (stream), ==, n_frames);

  oe_audio_output_close_stream (stream);
  g_free (frames);
}

static void
test_null_stream_tolerated (void)
{
  /* Every entry point documents NULL as a no-op. */
  g_assert_cmpuint (oe_audio_output_queue (NULL, NULL, 10), ==, 0);
  g_assert_cmpuint (oe_audio_output_queued_frames (NULL), ==, 0);
  oe_audio_output_flush (NULL);
  oe_audio_output_set_running (NULL, TRUE);
  oe_audio_output_close_stream (NULL);
}

static void
test_close_then_query_is_safe_via_null (void)
{
  ensure_audio_init ();

  GError *error = NULL;
  OeAudioDeviceInfo info;
  OeAudioStream *stream = oe_audio_output_open_stream (&info, &error);

  g_assert_nonnull (stream);
  oe_audio_output_close_stream (stream);

  /* After close the pointer is owned-and-freed; the contract's NULL
   * tolerance is what callers fall back to (see test_null_stream_tolerated).
   * Closing twice would be use-after-free, so the adapter contract ends
   * here — verified by the teardown symmetry below instead. */
  g_assert_true (oe_audio_output_is_initialized ());
}

static void
test_init_is_idempotent (void)
{
  ensure_audio_init ();
  ensure_audio_init (); /* second call is a no-op that still succeeds */
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  /* Shutdown-before-init and double-shutdown safety, at the edges of a
   * full init → use → shutdown cycle. */
  oe_audio_output_shutdown ();
  oe_audio_output_shutdown ();

  g_test_add_func ("/audio-output/open/requires-init", test_open_requires_init);
  g_test_add_func ("/audio-output/init/idempotent", test_init_is_idempotent);
  g_test_add_func ("/audio-output/open/reports-device-and-dummy",
                   test_open_reports_device_and_dummy);
  g_test_add_func ("/audio-output/queue/depth-and-flush", test_queue_depth_and_flush);
  g_test_add_func ("/audio-output/pause-resume/preserves-queue", test_pause_resume_preserves_queue);
  g_test_add_func ("/audio-output/null/tolerated", test_null_stream_tolerated);
  g_test_add_func ("/audio-output/close/then-initialized-still-true",
                   test_close_then_query_is_safe_via_null);

  const int result = g_test_run ();

  /* Reverse-order teardown: every stream was closed inside its test. */
  oe_audio_output_shutdown ();
  g_assert_false (oe_audio_output_is_initialized ());
  return result;
}
