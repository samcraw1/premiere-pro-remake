/* oe_import_worker.c — single-threaded import queue implementation (Phase 2).
 *
 * Threading model: exactly one worker GThread consumes refcounted,
 * immutable OeImportJob records from a GAsyncQueue. The main thread
 * never touches job state after submit except the worker-level atomic
 * cancel flag. Results are marshalled with g_main_context_invoke; the
 * completion callback and its data are copied into the dispatch closure
 * at post time, so freeing the worker cannot race a pending dispatch.
 */

#include "oe_import_worker.h"

#include <string.h>

#include "oe_log.h"
#include "oe_media_cache.h"

typedef struct
{
  grefcount refs;
  gchar *path;
  guint asset_id;
  gboolean relink;
} OeImportJob;

/* Queue terminator: g_async_queue_push rejects NULL, so we push the
 * address of a file-static byte — never a heap job, never freed. */
static char queue_sentinel;

typedef struct
{
  OeImportJob *job; /* owns one reference */
  OeImportDoneFunc done;
  gpointer done_data;
  OeImportJobResult res;
} Dispatch;

struct _OeImportWorker
{
  GThread *thread;
  GAsyncQueue *queue; /* OeImportJob* refs, terminated by the static sentinel */
  gint cancel;        /* atomic: main thread sets, worker clears and reads */
  OeImportDoneFunc done;
  gpointer done_data;
};

static gpointer worker_thread_main (gpointer data);

/* ---- refcounted immutable job ---- */

static OeImportJob *
job_new (const gchar *path, guint asset_id, gboolean relink)
{
  OeImportJob *job = g_new0 (OeImportJob, 1);

  g_ref_count_init (&job->refs);
  job->path = g_strdup (path);
  job->asset_id = asset_id;
  job->relink = relink;
  return job;
}

static OeImportJob *
job_ref (OeImportJob *job)
{
  g_ref_count_inc (&job->refs);
  return job;
}

static void
job_unref (OeImportJob *job)
{
  if (g_ref_count_dec (&job->refs))
    {
      g_free (job->path);
      g_free (job);
    }
}

/* ---- result marshalling ---- */

static void
dispatch_free (Dispatch *dispatch)
{
  job_unref (dispatch->job);
  g_clear_pointer (&dispatch->res.thumbnail.rgba, g_free);
  g_clear_pointer (&dispatch->res.waveform.peaks, g_free);
  oe_probe_info_clear (&dispatch->res.info);
  g_free (dispatch);
}

/* Runs on the main context (g_main_context_invoke from the worker). */
static gboolean
dispatch_on_main (gpointer data)
{
  Dispatch *dispatch = data;

  dispatch->done (&dispatch->res, dispatch->done_data);
  dispatch_free (dispatch);
  return G_SOURCE_REMOVE;
}

static void
post_result (OeImportWorker *worker, Dispatch *dispatch)
{
  dispatch->done = worker->done;
  dispatch->done_data = worker->done_data;
  g_main_context_invoke (NULL, dispatch_on_main, dispatch);
}

/* ---- cache entry format ----
 *
 * Each entry is a little-endian header followed by raw payload:
 *   thumbnail: magic "OET1", width, height, width*height*4 RGBA bytes
 *   waveform:  magic "OEW1", bucket_count, 2*bucket_count float bit
 *              patterns (min then max per bucket)
 * A short, truncated, or wrong-magic entry is treated as a miss.
 */

#define OE_CACHE_MAGIC_THUMB GUINT32_TO_LE (0x4f455431u) /* "OET1" */
#define OE_CACHE_MAGIC_WAVE GUINT32_TO_LE (0x4f455731u)  /* "OEW1" */

static void
put_u32 (guchar *p, guint32 v)
{
  p[0] = (guchar) (v & 0xff);
  p[1] = (guchar) ((v >> 8) & 0xff);
  p[2] = (guchar) ((v >> 16) & 0xff);
  p[3] = (guchar) ((v >> 24) & 0xff);
}

static guint32
get_u32 (const guchar *p)
{
  return (guint32) p[0] | ((guint32) p[1] << 8) | ((guint32) p[2] << 16) | ((guint32) p[3] << 24);
}

static void
put_float (guchar *p, gfloat f)
{
  guint32 bits;

  memcpy (&bits, &f, sizeof (bits));
  put_u32 (p, bits);
}

static gfloat
get_float (const guchar *p)
{
  guint32 bits = get_u32 (p);
  gfloat f;

  memcpy (&f, &bits, sizeof (f));
  return f;
}

static gchar *
cache_variant_key (const gchar *key, const gchar *suffix)
{
  return g_strdup_printf ("%s.%s", key, suffix);
}

static gboolean
load_cached_thumbnail (const gchar *key, OeThumbnail *out)
{
  g_return_val_if_fail (out->rgba == NULL, FALSE);

  gchar *variant = cache_variant_key (key, "thumb");
  guchar *data = NULL;
  gsize len = 0;
  gboolean hit = FALSE;

  if (oe_media_cache_lookup (variant, &data, &len) && len >= 12 && get_u32 (data) == OE_CACHE_MAGIC_THUMB)
    {
      guint32 width = get_u32 (data + 4);
      guint32 height = get_u32 (data + 8);

      if (width >= 1 && width <= OE_THUMBNAIL_BOX && height >= 1 && height <= OE_THUMBNAIL_BOX
          && len == 12 + (gsize) width * height * 4)
        {
          out->width = (gint) width;
          out->height = (gint) height;
          out->rgba = g_memdup2 (data + 12, (gsize) width * height * 4);
          hit = TRUE;
        }
    }

  g_free (data);
  g_free (variant);
  return hit;
}

static void
store_cached_thumbnail (const gchar *key, const OeThumbnail *thumb)
{
  gsize len = 12 + (gsize) thumb->width * thumb->height * 4;
  guchar *buf = g_malloc (len);

  put_u32 (buf, OE_CACHE_MAGIC_THUMB);
  put_u32 (buf + 4, (guint32) thumb->width);
  put_u32 (buf + 8, (guint32) thumb->height);
  memcpy (buf + 12, thumb->rgba, (gsize) thumb->width * thumb->height * 4);

  gchar *variant = cache_variant_key (key, "thumb");
  GError *error = NULL;

  if (!oe_media_cache_store (variant, buf, len, &error))
    {
      oe_log (OE_LOG_LEVEL_WARNING, "thumbnail cache store failed: %s", error->message);
      g_error_free (error);
    }

  g_free (variant);
  g_free (buf);
}

static gboolean
load_cached_waveform (const gchar *key, OeWaveform *out)
{
  g_return_val_if_fail (out->peaks == NULL, FALSE);

  gchar *variant = cache_variant_key (key, "wave");
  guchar *data = NULL;
  gsize len = 0;
  gboolean hit = FALSE;

  if (oe_media_cache_lookup (variant, &data, &len) && len >= 8 && get_u32 (data) == OE_CACHE_MAGIC_WAVE)
    {
      guint32 buckets = get_u32 (data + 4);

      if (buckets == (guint32) OE_WAVEFORM_BUCKETS && len == 8 + (gsize) buckets * 2 * 4)
        {
          out->bucket_count = (gint) buckets;
          out->peaks = g_new (gfloat, (gsize) buckets * 2);
          for (gsize i = 0; i < (gsize) buckets * 2; i++)
            out->peaks[i] = get_float (data + 8 + i * 4);
          hit = TRUE;
        }
    }

  g_free (data);
  g_free (variant);
  return hit;
}

static void
store_cached_waveform (const gchar *key, const OeWaveform *wave)
{
  gsize len = 8 + (gsize) wave->bucket_count * 2 * 4;
  guchar *buf = g_malloc (len);

  put_u32 (buf, OE_CACHE_MAGIC_WAVE);
  put_u32 (buf + 4, (guint32) wave->bucket_count);
  for (gint i = 0; i < wave->bucket_count * 2; i++)
    put_float (buf + 8 + (gsize) i * 4, wave->peaks[i]);

  gchar *variant = cache_variant_key (key, "wave");
  GError *error = NULL;

  if (!oe_media_cache_store (variant, buf, len, &error))
    {
      oe_log (OE_LOG_LEVEL_WARNING, "waveform cache store failed: %s", error->message);
      g_error_free (error);
    }

  g_free (variant);
  g_free (buf);
}

/* ---- job processing ---- */

static gboolean
cancel_check (gpointer user_data)
{
  OeImportWorker *worker = user_data;

  return g_atomic_int_get (&worker->cancel) != 0;
}

/* A decode failure after a successful probe means the stream metadata
 * promised content the decoders cannot deliver — the closest honest
 * verdict for the bin is UNSUPPORTED, matching how the UI treats it. */
static OeImportResult
decode_error_to_result (const GError *error)
{
  if (error != NULL && error->code == OE_MEDIA_JOB_ERROR_CANCELLED)
    return OE_IMPORT_RESULT_CANCELLED;

  return OE_IMPORT_RESULT_UNSUPPORTED;
}

static void
process_job (OeImportWorker *worker, OeImportJob *job)
{
  Dispatch *dispatch = g_new0 (Dispatch, 1);
  OeImportJobResult *res = &dispatch->res;

  dispatch->job = job_ref (job);
  res->asset_id = job->asset_id;
  res->relink = job->relink;

  GError *error = NULL;

  if (!oe_probe_file (job->path, &res->info, &error))
    {
      gboolean unsupported = error != NULL && error->code == OE_PROBE_ERROR_UNSUPPORTED;

      oe_log (OE_LOG_LEVEL_INFO, "probe failed for '%s' (%s): %s", job->path,
              unsupported ? "unsupported" : "missing", error != NULL ? error->message : "unknown");
      res->result = unsupported ? OE_IMPORT_RESULT_UNSUPPORTED : OE_IMPORT_RESULT_MISSING;
      g_error_free (error);
      post_result (worker, dispatch);
      return;
    }

  gchar *key = oe_media_cache_key_for_file (job->path, NULL);

  if (key == NULL)
    {
      /* Vanished between submit and processing: no stat, no key. */
      oe_log (OE_LOG_LEVEL_INFO, "import target vanished: '%s'", job->path);
      res->result = OE_IMPORT_RESULT_MISSING;
      post_result (worker, dispatch);
      return;
    }

  gboolean need_thumb = res->info.kind != OE_MEDIA_KIND_AUDIO;
  gboolean need_wave = res->info.channels > 0;
  gboolean all_cached = TRUE;

  if (need_thumb && load_cached_thumbnail (key, &res->thumbnail))
    oe_log (OE_LOG_LEVEL_DEBUG, "thumbnail cache hit for '%s'", job->path);
  else if (need_thumb)
    all_cached = FALSE;

  if (need_wave && load_cached_waveform (key, &res->waveform))
    oe_log (OE_LOG_LEVEL_DEBUG, "waveform cache hit for '%s'", job->path);
  else if (need_wave)
    all_cached = FALSE;

  if (!all_cached)
    {
      gboolean ok = TRUE;

      if (need_thumb && res->thumbnail.rgba == NULL)
        {
          ok = oe_media_job_thumbnail (job->path, cancel_check, worker, &res->thumbnail, &error);
          if (!ok)
            {
              res->result = decode_error_to_result (error);
              oe_log (OE_LOG_LEVEL_INFO, "thumbnail decode failed for '%s': %s", job->path,
                      error != NULL ? error->message : "cancelled");
              g_clear_error (&error);
            }
        }

      if (ok && need_wave && res->waveform.peaks == NULL)
        {
          ok = oe_media_job_waveform (job->path, cancel_check, worker, &res->waveform, &error);
          if (!ok)
            {
              res->result = decode_error_to_result (error);
              oe_log (OE_LOG_LEVEL_INFO, "waveform decode failed for '%s': %s", job->path,
                      error != NULL ? error->message : "cancelled");
              g_clear_error (&error);
            }
        }

      if (res->result == OE_IMPORT_RESULT_OK)
        {
          if (res->thumbnail.rgba != NULL)
            store_cached_thumbnail (key, &res->thumbnail);
          if (res->waveform.peaks != NULL)
            store_cached_waveform (key, &res->waveform);
        }
    }

  res->from_cache = all_cached;
  g_free (key);
  post_result (worker, dispatch);
}

static gpointer
worker_thread_main (gpointer data)
{
  OeImportWorker *worker = data;

  for (;;)
    {
      OeImportJob *job = g_async_queue_pop (worker->queue);

      if (job == (OeImportJob *) &queue_sentinel)
        break; /* sentinel: shutdown drain */

      g_atomic_int_set (&worker->cancel, 0);
      process_job (worker, job);
      job_unref (job);
    }

  return NULL;
}

/* ---- public API ---- */

const gchar *
oe_import_result_get_name (OeImportResult result)
{
  switch (result)
    {
    case OE_IMPORT_RESULT_OK:
      return "OK";
    case OE_IMPORT_RESULT_UNSUPPORTED:
      return "Unsupported";
    case OE_IMPORT_RESULT_MISSING:
      return "Missing";
    case OE_IMPORT_RESULT_CANCELLED:
      return "Cancelled";
    default:
      return "Unknown";
    }
}

OeImportWorker *
oe_import_worker_new (OeImportDoneFunc done, gpointer user_data)
{
  g_return_val_if_fail (done != NULL, NULL);

  OeImportWorker *worker = g_new0 (OeImportWorker, 1);

  worker->queue = g_async_queue_new ();
  worker->done = done;
  worker->done_data = user_data;
  worker->thread = g_thread_new ("oe-import", worker_thread_main, worker);
  return worker;
}

void
oe_import_worker_submit (OeImportWorker *worker, const gchar *path, guint asset_id,
                         gboolean relink)
{
  g_return_if_fail (worker != NULL);
  g_return_if_fail (path != NULL && path[0] != '\0');

  g_async_queue_push (worker->queue, job_new (path, asset_id, relink));
}

void
oe_import_worker_cancel_current (OeImportWorker *worker)
{
  g_return_if_fail (worker != NULL);

  g_atomic_int_set (&worker->cancel, 1);
}

void
oe_import_worker_free (OeImportWorker *worker)
{
  if (worker == NULL)
    return;

  /* Drain: cancel hurries the in-flight job, the sentinel ends the
   * loop once every queued job has been processed, and the join waits
   * for both. Called BEFORE oe_ffmpeg_shutdown by the owner. */
  g_atomic_int_set (&worker->cancel, 1);
  g_async_queue_push (worker->queue, &queue_sentinel);
  g_thread_join (worker->thread);
  g_async_queue_unref (worker->queue);

  /* Deliver results already invoked onto the main context while the
   * owner's user_data is still valid. */
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_free (worker);
}
