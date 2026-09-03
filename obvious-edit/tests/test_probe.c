/* test_probe.c — GTK-free tests for stream metadata probing (Phase 2).
 *
 * Fixtures are generated at runtime with known parameters, so probe output
 * is asserted exactly — tolerance only on container-reported duration.
 *
 *   /probe/video-fixture      MJPEG-in-AVI: kind, container, duration
 *                             (tolerance), dimensions, rational frame
 *                             rate, codec names, no audio fields.
 *   /probe/audio-fixture      WAV pcm_s16le: kind, container, duration,
 *                             sample rate, channels, codec, no video
 *                             fields.
 *   /probe/still-fixture      PNG: kind, dimensions, codec, zero duration.
 *   /probe/missing-file       nonexistent path → OPEN_FAILED.
 *   /probe/text-file          garbage text → OPEN_FAILED.
 *   /probe/empty-file         zero bytes → OPEN_FAILED.
 *   /probe/clear-is-idempotent  clear on a probed record, then again.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include <string.h>

#include "../src/media/oe_probe.h"
#include "fixture_media.h"

static OeFixtures fixtures;

static void
fixture_set_up (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  g_assert_true (oe_fixtures_create (fx, &error));
  g_assert_no_error (error);
  memcpy (&fixtures, fx, sizeof (fixtures));
}

static void
fixture_tear_down (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  oe_fixtures_free (fx);
  memset (&fixtures, 0, sizeof (fixtures));
}

static void
test_video_fixture (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  OeProbeInfo info;

  oe_probe_info_init (&info);

  GError *error = NULL;

  g_assert_true (oe_probe_file (fx->avi_path, &info, &error));
  g_assert_no_error (error);

  g_assert_cmpint (info.kind, ==, OE_MEDIA_KIND_VIDEO);
  g_assert_nonnull (info.container_name);
  g_assert_cmpstr (info.container_name, ==, "avi");
  g_assert_cmpint (info.width, ==, OE_FIXTURE_AVI_WIDTH);
  g_assert_cmpint (info.height, ==, OE_FIXTURE_AVI_HEIGHT);

  /* 30 frames at 25 fps; the container rounds, the test tolerates. */
  g_assert_cmpint (info.duration_us, >, OE_FIXTURE_AVI_DURATION_US - 100000);
  g_assert_cmpint (info.duration_us, <=, OE_FIXTURE_AVI_DURATION_US + 100000);

  g_assert_cmpint (info.frame_rate_num, ==, OE_FIXTURE_AVI_FPS_NUM);
  g_assert_cmpint (info.frame_rate_den, ==, OE_FIXTURE_AVI_FPS_DEN);

  g_assert_cmpstr (info.video_codec, ==, "mjpeg");
  g_assert_null (info.audio_codec);
  g_assert_cmpint (info.sample_rate, ==, 0);
  g_assert_cmpint (info.channels, ==, 0);

  oe_probe_info_clear (&info);
}

static void
test_audio_fixture (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  OeProbeInfo info;
  GError *error = NULL;

  oe_probe_info_init (&info);
  g_assert_true (oe_probe_file (fx->wav_path, &info, &error));
  g_assert_no_error (error);

  g_assert_cmpint (info.kind, ==, OE_MEDIA_KIND_AUDIO);
  g_assert_cmpstr (info.container_name, ==, "wav");

  g_assert_cmpint (info.duration_us, >, OE_FIXTURE_WAV_DURATION_US - 100000);
  g_assert_cmpint (info.duration_us, <=, OE_FIXTURE_WAV_DURATION_US + 100000);

  g_assert_cmpint (info.sample_rate, ==, OE_FIXTURE_WAV_RATE);
  g_assert_cmpint (info.channels, ==, OE_FIXTURE_WAV_CHANNELS);
  g_assert_cmpstr (info.audio_codec, ==, "pcm_s16le");

  g_assert_null (info.video_codec);
  g_assert_cmpint (info.width, ==, 0);
  g_assert_cmpint (info.height, ==, 0);
  g_assert_cmpint (info.frame_rate_num, ==, 0);
  g_assert_cmpint (info.frame_rate_den, ==, 0);

  oe_probe_info_clear (&info);
}

static void
test_still_fixture (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  OeProbeInfo info;
  GError *error = NULL;

  oe_probe_info_init (&info);
  g_assert_true (oe_probe_file (fx->png_path, &info, &error));
  g_assert_no_error (error);

  g_assert_cmpint (info.kind, ==, OE_MEDIA_KIND_STILL_IMAGE);
  g_assert_nonnull (info.container_name);
  g_assert_cmpint (info.width, ==, OE_FIXTURE_PNG_WIDTH);
  g_assert_cmpint (info.height, ==, OE_FIXTURE_PNG_HEIGHT);
  g_assert_cmpstr (info.video_codec, ==, "png");
  g_assert_null (info.audio_codec);

  /* Stills carry no duration and no meaningful frame rate. */
  g_assert_cmpint (info.duration_us, ==, 0);

  oe_probe_info_clear (&info);
}

static void
test_missing_file (OeFixtures *fx G_GNUC_UNUSED, gconstpointer user_data G_GNUC_UNUSED)
{
  OeProbeInfo info;
  GError *error = NULL;

  oe_probe_info_init (&info);
  g_assert_false (oe_probe_file ("/nonexistent/path/nothing.avi", &info, &error));
  g_assert_error (error, OE_PROBE_ERROR, OE_PROBE_ERROR_OPEN_FAILED);
  g_clear_error (&error);
  oe_probe_info_clear (&info);
}

static void
test_text_file (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  OeProbeInfo info;
  GError *error = NULL;

  oe_probe_info_init (&info);
  g_assert_false (oe_probe_file (fx->txt_path, &info, &error));
  g_assert_error (error, OE_PROBE_ERROR, OE_PROBE_ERROR_OPEN_FAILED);
  g_clear_error (&error);
  oe_probe_info_clear (&info);
}

static void
test_empty_file (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  OeProbeInfo info;
  GError *error = NULL;

  oe_probe_info_init (&info);
  g_assert_false (oe_probe_file (fx->empty_path, &info, &error));
  g_assert_error (error, OE_PROBE_ERROR, OE_PROBE_ERROR_OPEN_FAILED);
  g_clear_error (&error);
  oe_probe_info_clear (&info);
}

static void
test_clear_is_idempotent (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  OeProbeInfo info;
  GError *error = NULL;

  oe_probe_info_init (&info);
  g_assert_true (oe_probe_file (fx->avi_path, &info, &error));
  g_assert_no_error (error);

  oe_probe_info_clear (&info);
  oe_probe_info_clear (&info); /* second clear on a zeroed record: no crash */

  g_assert_null (info.container_name);
  g_assert_null (info.video_codec);
  g_assert_null (info.audio_codec);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/probe/video-fixture", OeFixtures, NULL, fixture_set_up, test_video_fixture,
              fixture_tear_down);
  g_test_add ("/probe/audio-fixture", OeFixtures, NULL, fixture_set_up, test_audio_fixture,
              fixture_tear_down);
  g_test_add ("/probe/still-fixture", OeFixtures, NULL, fixture_set_up, test_still_fixture,
              fixture_tear_down);
  g_test_add ("/probe/missing-file", OeFixtures, NULL, fixture_set_up, test_missing_file,
              fixture_tear_down);
  g_test_add ("/probe/text-file", OeFixtures, NULL, fixture_set_up, test_text_file,
              fixture_tear_down);
  g_test_add ("/probe/empty-file", OeFixtures, NULL, fixture_set_up, test_empty_file,
              fixture_tear_down);
  g_test_add ("/probe/clear-is-idempotent", OeFixtures, NULL, fixture_set_up,
              test_clear_is_idempotent, fixture_tear_down);

  return g_test_run ();
}
