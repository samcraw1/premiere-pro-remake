/* test_import_worker.c — GTK-free tests for the import queue (Phase 2).
 *
 *   /worker/ok-completion       AVI submitted from a GMainLoop completes
 *                               on the main context: OK verdict, probed
 *                               metadata, downscaled thumbnail, no
 *                               waveform (no audio), from_cache FALSE.
 *   /worker/cache-hit           the same file submitted again skips
 *                               decode entirely: identical thumbnail
 *                               bytes with from_cache TRUE.
 *   /worker/wav-completion      audio-only WAV: OK with peaks and no
 *                               thumbnail.
 *   /worker/missing-file        a text file reports MISSING; an unknown
 *                               id is preserved in the result.
 *   /worker/relink-flag         the relink flag round-trips to the
 *                               completion callback.
 *
 * Cancellation is intentionally not driven here: cancel delivery is
 * timing-dependent (the unit-tested cancel check in test_media_jobs
 * covers the job-level contract), and a race-sensitive assertion would
 * flake in CI.
 */

#include <string.h>

#include <glib.h>

#include "../src/app/oe_import_worker.h"
#include "fixture_media.h"

typedef struct
{
  GMainLoop *loop;
  gboolean got;
  gboolean failed;
  OeImportJobResult last;
} DoneCtx;

/* The result is owned by the dispatch and valid only during the call:
 * deep-copy what the test still needs after the loop returns. */
static void
result_copy (OeImportJobResult *dst, const OeImportJobResult *src)
{
  *dst = *src;

  /* Drop the shallow string pointers before deep-copying, or the copy
   * step would free strings the source still owns. */
  memset (&dst->info, 0, sizeof dst->info);
  oe_probe_info_copy (&dst->info, &src->info);
  if (src->thumbnail.rgba != NULL)
    dst->thumbnail.rgba = g_memdup2 (src->thumbnail.rgba,
                                     (gsize) src->thumbnail.width * src->thumbnail.height * 4);
  if (src->waveform.peaks != NULL)
    dst->waveform.peaks = g_memdup2 (src->waveform.peaks,
                                     (gsize) src->waveform.bucket_count * 2 * sizeof (gfloat));
}

static void
result_clear (OeImportJobResult *res)
{
  oe_probe_info_clear (&res->info);
  g_clear_pointer (&res->thumbnail.rgba, g_free);
  g_clear_pointer (&res->waveform.peaks, g_free);
}

static void
on_done (const OeImportJobResult *result, gpointer user_data)
{
  DoneCtx *ctx = user_data;

  result_copy (&ctx->last, result);
  ctx->got = TRUE;
  g_main_loop_quit (ctx->loop);
}

/* Timeout guard: a stuck worker must fail the test, not hang CI. */
static gboolean
on_timeout (gpointer user_data)
{
  DoneCtx *ctx = user_data;

  ctx->failed = TRUE;
  g_main_loop_quit (ctx->loop);
  return G_SOURCE_REMOVE;
}

static void
run_and_wait (OeImportWorker *worker, DoneCtx *ctx, const gchar *path, guint asset_id,
              gboolean relink)
{
  ctx->got = FALSE;
  ctx->failed = FALSE;
  result_clear (&ctx->last);

  guint timeout_id = g_timeout_add_seconds (30, on_timeout, ctx);

  oe_import_worker_submit (worker, path, asset_id, relink);
  g_main_loop_run (ctx->loop);
  g_source_remove (timeout_id);

  g_assert_false (ctx->failed);
  g_assert_true (ctx->got);
}

typedef struct
{
  OeFixtures fx;
  gchar *cache_dir;
} WorkerFixture;

static void
worker_fixture_setup (WorkerFixture *wfx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  g_assert_true (oe_fixtures_create (&wfx->fx, &error));
  g_assert_no_error (error);

  wfx->cache_dir = g_dir_make_tmp ("oe-worker-cache-XXXXXX", &error);
  g_assert_no_error (error);
  g_setenv ("OE_MEDIA_CACHE_DIR", wfx->cache_dir, TRUE);
}

static void
worker_fixture_teardown (WorkerFixture *wfx, gconstpointer user_data G_GNUC_UNUSED)
{
  oe_fixtures_free (&wfx->fx);
  g_free (wfx->cache_dir);
}

static void
test_ok_completion (WorkerFixture *wfx, gconstpointer user_data G_GNUC_UNUSED)
{
  DoneCtx ctx = { NULL, FALSE, FALSE, { 0 } };

  ctx.loop = g_main_loop_new (NULL, FALSE);

  OeImportWorker *worker = oe_import_worker_new (on_done, &ctx);

  run_and_wait (worker, &ctx, wfx->fx.avi_path, 7, FALSE);

  g_assert_cmpint (ctx.last.result, ==, OE_IMPORT_RESULT_OK);
  g_assert_false (ctx.last.from_cache);
  g_assert_false (ctx.last.relink);
  g_assert_cmpuint (ctx.last.asset_id, ==, 7);

  /* Probed metadata matches the fixture pins. */
  g_assert_cmpint (ctx.last.info.kind, ==, OE_MEDIA_KIND_VIDEO);
  g_assert_cmpint (ctx.last.info.width, ==, OE_FIXTURE_AVI_WIDTH);
  g_assert_cmpint (ctx.last.info.height, ==, OE_FIXTURE_AVI_HEIGHT);
  g_assert_cmpint (ctx.last.info.frame_rate_num, ==, OE_FIXTURE_AVI_FPS_NUM);
  g_assert_cmpint (ctx.last.info.frame_rate_den, ==, OE_FIXTURE_AVI_FPS_DEN);

  /* 192x108 downscales into the 96 box: exactly 96x54. */
  g_assert_cmpint (ctx.last.thumbnail.width, ==, OE_THUMBNAIL_BOX);
  g_assert_cmpint (ctx.last.thumbnail.height, ==, 54);
  g_assert_nonnull (ctx.last.thumbnail.rgba);

  /* No audio track: no waveform. */
  g_assert_null (ctx.last.waveform.peaks);

  result_clear (&ctx.last);
  g_main_loop_unref (ctx.loop);
  oe_import_worker_free (worker);
}

static void
test_cache_hit (WorkerFixture *wfx, gconstpointer user_data G_GNUC_UNUSED)
{
  DoneCtx ctx = { NULL, FALSE, FALSE, { 0 } };

  ctx.loop = g_main_loop_new (NULL, FALSE);

  OeImportWorker *worker = oe_import_worker_new (on_done, &ctx);

  /* First submission: a miss, so the decode runs. */
  run_and_wait (worker, &ctx, wfx->fx.avi_path, 1, FALSE);

  g_assert_cmpint (ctx.last.result, ==, OE_IMPORT_RESULT_OK);
  g_assert_false (ctx.last.from_cache);
  g_assert_cmpint (ctx.last.thumbnail.width, ==, OE_THUMBNAIL_BOX);

  const gint cached_width = ctx.last.thumbnail.width;
  const gint cached_height = ctx.last.thumbnail.height;
  const gsize cached_len = (gsize) cached_width * cached_height * 4;
  guchar *cached_bytes = g_memdup2 (ctx.last.thumbnail.rgba, cached_len);

  result_clear (&ctx.last);

  /* Second submission: the cache serves the thumbnail, no decode. */
  run_and_wait (worker, &ctx, wfx->fx.avi_path, 2, FALSE);

  g_assert_cmpint (ctx.last.result, ==, OE_IMPORT_RESULT_OK);
  g_assert_true (ctx.last.from_cache);
  g_assert_cmpint (ctx.last.thumbnail.width, ==, cached_width);
  g_assert_cmpint (ctx.last.thumbnail.height, ==, cached_height);
  g_assert_cmpint (memcmp (ctx.last.thumbnail.rgba, cached_bytes, cached_len), ==, 0);

  g_free (cached_bytes);
  result_clear (&ctx.last);
  g_main_loop_unref (ctx.loop);
  oe_import_worker_free (worker);
}

static void
test_wav_completion (WorkerFixture *wfx, gconstpointer user_data G_GNUC_UNUSED)
{
  DoneCtx ctx = { NULL, FALSE, FALSE, { 0 } };

  ctx.loop = g_main_loop_new (NULL, FALSE);

  OeImportWorker *worker = oe_import_worker_new (on_done, &ctx);

  run_and_wait (worker, &ctx, wfx->fx.wav_path, 3, FALSE);

  g_assert_cmpint (ctx.last.result, ==, OE_IMPORT_RESULT_OK);
  g_assert_false (ctx.last.from_cache);
  g_assert_cmpint (ctx.last.info.kind, ==, OE_MEDIA_KIND_AUDIO);
  g_assert_cmpint (ctx.last.info.sample_rate, ==, OE_FIXTURE_WAV_RATE);
  g_assert_cmpint (ctx.last.info.channels, ==, OE_FIXTURE_WAV_CHANNELS);

  /* Audio-only: peaks produced, no thumbnail. */
  g_assert_nonnull (ctx.last.waveform.peaks);
  g_assert_cmpint (ctx.last.waveform.bucket_count, ==, OE_WAVEFORM_BUCKETS);
  g_assert_null (ctx.last.thumbnail.rgba);

  result_clear (&ctx.last);
  g_main_loop_unref (ctx.loop);
  oe_import_worker_free (worker);
}

static void
test_missing_file (WorkerFixture *wfx, gconstpointer user_data G_GNUC_UNUSED)
{
  DoneCtx ctx = { NULL, FALSE, FALSE, { 0 } };

  ctx.loop = g_main_loop_new (NULL, FALSE);

  OeImportWorker *worker = oe_import_worker_new (on_done, &ctx);

  run_and_wait (worker, &ctx, wfx->fx.txt_path, 42, FALSE);

  g_assert_cmpint (ctx.last.result, ==, OE_IMPORT_RESULT_MISSING);
  g_assert_cmpuint (ctx.last.asset_id, ==, 42);
  g_assert_false (ctx.last.from_cache);

  result_clear (&ctx.last);
  g_main_loop_unref (ctx.loop);
  oe_import_worker_free (worker);
}

static void
test_relink_flag (WorkerFixture *wfx, gconstpointer user_data G_GNUC_UNUSED)
{
  DoneCtx ctx = { NULL, FALSE, FALSE, { 0 } };

  ctx.loop = g_main_loop_new (NULL, FALSE);

  OeImportWorker *worker = oe_import_worker_new (on_done, &ctx);

  run_and_wait (worker, &ctx, wfx->fx.png_path, 5, TRUE);

  g_assert_cmpint (ctx.last.result, ==, OE_IMPORT_RESULT_OK);
  g_assert_true (ctx.last.relink);
  g_assert_cmpint (ctx.last.info.kind, ==, OE_MEDIA_KIND_STILL_IMAGE);
  g_assert_cmpint (ctx.last.info.width, ==, OE_FIXTURE_PNG_WIDTH);
  g_assert_cmpint (ctx.last.info.height, ==, OE_FIXTURE_PNG_HEIGHT);

  result_clear (&ctx.last);
  g_main_loop_unref (ctx.loop);
  oe_import_worker_free (worker);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/worker/ok-completion", WorkerFixture, NULL, worker_fixture_setup,
              test_ok_completion, worker_fixture_teardown);
  g_test_add ("/worker/cache-hit", WorkerFixture, NULL, worker_fixture_setup, test_cache_hit,
              worker_fixture_teardown);
  g_test_add ("/worker/wav-completion", WorkerFixture, NULL, worker_fixture_setup,
              test_wav_completion, worker_fixture_teardown);
  g_test_add ("/worker/missing-file", WorkerFixture, NULL, worker_fixture_setup,
              test_missing_file, worker_fixture_teardown);
  g_test_add ("/worker/relink-flag", WorkerFixture, NULL, worker_fixture_setup, test_relink_flag,
              worker_fixture_teardown);

  return g_test_run ();
}
