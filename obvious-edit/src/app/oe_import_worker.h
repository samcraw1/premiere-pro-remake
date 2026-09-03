/* oe_import_worker.h — single-threaded import queue (Phase 2).
 *
 * Owns the one decode thread of the application: jobs are submitted on
 * the main thread, run on the worker thread, and their results are
 * marshalled back with g_main_context_invoke so the completion callback
 * always runs on the main context. FFmpeg init/shutdown stays outside
 * this module; the owner frees the worker (drain + join) BEFORE calling
 * oe_ffmpeg_shutdown(), honoring the app's reverse-order teardown.
 *
 * Results pass through the media cache: before decoding, the worker
 * looks the file up by cache key (canonical path + size + mtime) and
 * reports OE_IMPORT_RESULT_OK with from_cache = TRUE on a hit.
 */

#pragma once

#include <glib.h>

#include "../media/oe_media_jobs.h"
#include "../media/oe_probe.h"

G_BEGIN_DECLS

/**
 * OeImportResult:
 * @OE_IMPORT_RESULT_OK: probed and derived media produced (or cached).
 * @OE_IMPORT_RESULT_UNSUPPORTED: container opened but has no decodable
 *   audio/video stream.
 * @OE_IMPORT_RESULT_MISSING: file unopenable at import time.
 * @OE_IMPORT_RESULT_CANCELLED: cancellation observed between decode
 *   steps; the row stays pending for another import.
 */
typedef enum
{
  OE_IMPORT_RESULT_OK,
  OE_IMPORT_RESULT_UNSUPPORTED,
  OE_IMPORT_RESULT_MISSING,
  OE_IMPORT_RESULT_CANCELLED,
} OeImportResult;

const gchar *oe_import_result_get_name (OeImportResult result);

/**
 * OeImportJobResult: what the completion callback receives.
 * @asset_id: opaque library id the job was submitted with.
 * @relink: TRUE when the job re-probed an existing bin row.
 * @result: outcome of the job.
 * @info: probed metadata; meaningful only when @result is OK.
 * @thumbnail: decoded or cached RGBA thumbnail; meaningful only when
 *   @result is OK and the file carries video or is a still.
 * @waveform: decoded or cached peaks; meaningful only when @result is
 *   OK and the file carries audio.
 * @from_cache: TRUE when no decode ran (cache hit).
 */
typedef struct
{
  guint asset_id;
  gboolean relink;
  OeImportResult result;
  OeProbeInfo info;
  OeThumbnail thumbnail;
  OeWaveform waveform;
  gboolean from_cache;
} OeImportJobResult;

/**
 * OeImportDoneFunc: completion callback, always invoked on the main
 * context (default context of the thread that created the worker).
 *
 * @result: owned copy, valid only during the call.
 * @user_data: context pointer supplied at worker creation.
 */
typedef void (*OeImportDoneFunc) (const OeImportJobResult *result, gpointer user_data);

typedef struct _OeImportWorker OeImportWorker;

/**
 * oe_import_worker_new:
 * @done: completion callback (required).
 * @user_data: passed to @done.
 *
 * Spawns the worker thread immediately. Requires the default main
 * context to exist (worker creation happens after gtk_init in app
 * startup).
 */
OeImportWorker *oe_import_worker_new (OeImportDoneFunc done, gpointer user_data);

/**
 * oe_import_worker_free:
 *
 * Drains the queue (sentinel), cancels and joins the worker thread,
 * then delivers any result already queued on the main context. Must be
 * called on the main thread and BEFORE oe_ffmpeg_shutdown().
 */
void oe_import_worker_free (OeImportWorker *worker);

/**
 * oe_import_worker_submit:
 * @path: media file path (copied).
 * @asset_id: library record the result will be applied to.
 * @relink: TRUE for a re-probe of an existing row.
 *
 * Enqueues an immutable job. Results arrive via the @done callback.
 */
void oe_import_worker_submit (OeImportWorker *worker, const gchar *path, guint asset_id,
                              gboolean relink);

/**
 * oe_import_worker_cancel_current:
 *
 * Sets the atomic cancel flag consulted between decode steps. At most
 * the in-flight job observes it; the flag resets for the next job, so
 * a cancel racing a job's tail may simply be lost — the result reports
 * the outcome as computed.
 */
void oe_import_worker_cancel_current (OeImportWorker *worker);

G_END_DECLS
