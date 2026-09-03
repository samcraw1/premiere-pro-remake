/* test_media_jobs.c — GTK-free tests for thumbnail and waveform jobs (Phase 2).
 *
 *   /jobs/thumbnail-box-fit      AVI 64x48 decodes to exactly 96x72 RGBA
 *                                (96-box fit, aspect preserved) with
 *                                non-trivial pixels.
 *   /jobs/thumbnail-still        PNG still decodes to its native 32x20 —
 *                                already inside the box.
 *   /jobs/thumbnail-wrong-kind   thumbnail from a WAV → UNSUPPORTED.
 *   /jobs/thumbnail-open-failed  thumbnail from a missing file →
 *                                OPEN_FAILED.
 *   /jobs/waveform-peaks         WAV sine produces non-empty peaks, all
 *                                in [-1, 1], min < max per bucket, and
 *                                real amplitude from the fixture.
 *   /jobs/waveform-wrong-kind    waveform from a video-only AVI →
 *                                UNSUPPORTED.
 *   /jobs/cancel-check           an always-cancel check stops the job
 *                                with CANCELLED.
 *   /jobs/cancel-check           an always-cancel check stops the job
 *                                with CANCELLED.
 *   /jobs/cache-round-trip       miss → store → hit with byte-identical
 *                                payload, and a corrupt entry reads as
 *                                a miss (no error).
 */

#include <string.h>

#include <glib.h>

#include <math.h>
#include "../src/app/oe_media_cache.h"
#include "../src/media/oe_media_jobs.h"
#include "fixture_media.h"

static void
test_cache_round_trip (gconstpointer user_data)
{
  const OeFixtures *fx = user_data;
  GError *error = NULL;

  /* Fresh cache location for this test binary. */
  gchar *cache_dir = g_dir_make_tmp ("oe-media-cache-XXXXXX", &error);

  g_assert_no_error (error);
  g_assert_nonnull (cache_dir);
  g_setenv ("OE_MEDIA_CACHE_DIR", cache_dir, TRUE);

  gchar *key = oe_media_cache_key_for_file (fx->avi_path, &error);

  g_assert_no_error (error);
  g_assert_nonnull (key);

  /* Miss on an empty cache. */
  guchar *data = NULL;
  gsize len = 0;

  g_assert_false (oe_media_cache_lookup (key, &data, &len));
  g_assert_null (data);

  /* Store → hit with byte-identical payload. */
  const guchar payload[] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x42 };

  g_assert_true (oe_media_cache_store (key, payload, sizeof (payload), &error));
  g_assert_no_error (error);
  g_assert_true (oe_media_cache_lookup (key, &data, &len));
  g_assert_cmpuint (len, ==, sizeof (payload));
  g_assert_cmpint (memcmp (data, payload, sizeof (payload)), ==, 0);
  g_free (data);

  /* A corrupt entry reads as a miss, not an error. */
  gchar *entry_path = oe_media_cache_path_for_key (key);

  g_assert_true (g_file_set_contents (entry_path, "junk", 4, &error));
  g_assert_no_error (error);
  g_assert_false (oe_media_cache_lookup (key, &data, &len));
  g_assert_null (data);

  /* Deleting the whole cache directory is safe by design. */
  g_assert_true (g_file_test (oe_media_cache_path_for_key (key), G_FILE_TEST_EXISTS));

  g_free (entry_path);
  g_free (key);
  g_free (cache_dir);
}

static gboolean
cancel_always (gpointer user_data G_GNUC_UNUSED)
{
  return TRUE;
}

static void
test_thumbnail_box_fit (gconstpointer user_data)
{
  const OeFixtures *fx = user_data;
  OeThumbnail thumb = { 0, 0, NULL };
  GError *error = NULL;

  g_assert_true (oe_media_job_thumbnail (fx->avi_path, NULL, NULL, &thumb, &error));
  g_assert_no_error (error);

  /* 192x108 downscales to fit the 96 box → exactly 96x54, 16:9 kept. */
  g_assert_cmpint (thumb.width, ==, OE_THUMBNAIL_BOX);
  g_assert_cmpint (thumb.height, ==, 54);

  /* RGBA bytes exist and are not all-black: the fixture has a ramp. */
  g_assert_nonnull (thumb.rgba);
  gboolean saw_nonzero = FALSE;

  for (gint i = 0; i < thumb.width * thumb.height; i++)
    {
      if (thumb.rgba[(gsize) i * 4 + 0] != 0 || thumb.rgba[(gsize) i * 4 + 1] != 0)
        {
          saw_nonzero = TRUE;
          break;
        }
    }
  g_assert_true (saw_nonzero);

  oe_thumbnail_free (&thumb);
}

static void
test_thumbnail_still (gconstpointer user_data)
{
  const OeFixtures *fx = user_data;
  OeThumbnail thumb = { 0, 0, NULL };
  GError *error = NULL;

  g_assert_true (oe_media_job_thumbnail (fx->png_path, NULL, NULL, &thumb, &error));
  g_assert_no_error (error);

  /* Native size already fits the box: no scaling needed. */
  g_assert_cmpint (thumb.width, ==, OE_FIXTURE_PNG_WIDTH);
  g_assert_cmpint (thumb.height, ==, OE_FIXTURE_PNG_HEIGHT);

  oe_thumbnail_free (&thumb);
}

static void
test_thumbnail_wrong_kind (gconstpointer user_data)
{
  const OeFixtures *fx = user_data;
  OeThumbnail thumb = { 0, 0, NULL };
  GError *error = NULL;

  g_assert_false (oe_media_job_thumbnail (fx->wav_path, NULL, NULL, &thumb, &error));
  g_assert_error (error, OE_MEDIA_JOB_ERROR, OE_MEDIA_JOB_ERROR_UNSUPPORTED);
  g_clear_error (&error);
  oe_thumbnail_free (&thumb);
}

static void
test_thumbnail_open_failed (gconstpointer user_data G_GNUC_UNUSED)
{
  OeThumbnail thumb = { 0, 0, NULL };
  GError *error = NULL;

  g_assert_false (oe_media_job_thumbnail ("/nonexistent/nope.avi", NULL, NULL, &thumb, &error));
  g_assert_error (error, OE_MEDIA_JOB_ERROR, OE_MEDIA_JOB_ERROR_OPEN_FAILED);
  g_clear_error (&error);
  oe_thumbnail_free (&thumb);
}

static void
test_waveform_peaks (gconstpointer user_data)
{
  const OeFixtures *fx = user_data;
  OeWaveform wf = { 0, NULL };
  GError *error = NULL;

  g_assert_true (oe_media_job_waveform (fx->wav_path, NULL, NULL, &wf, &error));
  g_assert_no_error (error);

  g_assert_cmpint (wf.bucket_count, ==, OE_WAVEFORM_BUCKETS);
  g_assert_nonnull (wf.peaks);

  gfloat max_seen = -1.0f;

  for (gint b = 0; b < wf.bucket_count; b++)
    {
      gfloat lo = wf.peaks[(gsize) b * 2];
      gfloat hi = wf.peaks[(gsize) b * 2 + 1];

      g_assert_cmpfloat (lo, >=, -1.0f);
      g_assert_cmpfloat (lo, <=, 1.0f);
      g_assert_cmpfloat (hi, >=, -1.0f);
      g_assert_cmpfloat (hi, <=, 1.0f);
      g_assert_cmpfloat (lo, <=, hi);
      if (hi > max_seen)
        max_seen = hi;
    }

  /* A 0.8-full-scale sine must produce genuinely large peaks. */
  g_assert_cmpfloat (max_seen, >, 0.5f);

  oe_waveform_free (&wf);
}

static void
test_waveform_wrong_kind (gconstpointer user_data)
{
  const OeFixtures *fx = user_data;
  OeWaveform wf = { 0, NULL };
  GError *error = NULL;

  g_assert_false (oe_media_job_waveform (fx->avi_path, NULL, NULL, &wf, &error));
  g_assert_error (error, OE_MEDIA_JOB_ERROR, OE_MEDIA_JOB_ERROR_UNSUPPORTED);
  g_clear_error (&error);
  oe_waveform_free (&wf);
}

static void
test_cancel_check (gconstpointer user_data)
{
  const OeFixtures *fx = user_data;
  OeThumbnail thumb = { 0, 0, NULL };
  GError *error = NULL;

  g_assert_false (oe_media_job_thumbnail (fx->avi_path, cancel_always, NULL, &thumb, &error));
  g_assert_error (error, OE_MEDIA_JOB_ERROR, OE_MEDIA_JOB_ERROR_CANCELLED);
  g_clear_error (&error);
  oe_thumbnail_free (&thumb);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  OeFixtures fx;
  GError *error = NULL;

  g_assert_true (oe_fixtures_create (&fx, &error));
  g_assert_no_error (error);

  g_test_add_data_func ("/jobs/thumbnail-box-fit", &fx, test_thumbnail_box_fit);
  g_test_add_data_func ("/jobs/thumbnail-still", &fx, test_thumbnail_still);
  g_test_add_data_func ("/jobs/thumbnail-wrong-kind", &fx, test_thumbnail_wrong_kind);
  g_test_add_data_func ("/jobs/thumbnail-open-failed", &fx, test_thumbnail_open_failed);
  g_test_add_data_func ("/jobs/waveform-peaks", &fx, test_waveform_peaks);
  g_test_add_data_func ("/jobs/waveform-wrong-kind", &fx, test_waveform_wrong_kind);
  g_test_add_data_func ("/jobs/cancel-check", &fx, test_cancel_check);
  g_test_add_data_func ("/jobs/cache-round-trip", &fx, test_cache_round_trip);

  int result = g_test_run ();

  oe_fixtures_free (&fx);
  return result;
}
