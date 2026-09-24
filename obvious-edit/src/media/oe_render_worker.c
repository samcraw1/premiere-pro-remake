/* oe_render_worker.c — off-main-thread monitor frame rendering.
 *
 * One GThread, one mutex/cond pair, two single-slot mailboxes: the
 * pending snapshot and the pending render request. Both are "latest
 * wins" — the worker only ever needs the newest sequence and the newest
 * frame the transport asked for. Deliveries are queued onto the main
 * context as idle sources, exactly like the audio worker's chunks.
 *
 * Everything FFmpeg-shaped (the render session, its decoders) is
 * created, used, and destroyed on the worker thread.
 */

#include "oe_render_worker.h"

#include "../app/oe_log.h"
#include "oe_render.h"

/* A sequence snapshot the worker owns outright: the deep copy, the
 * resolved path table, and the render source built over them. */
typedef struct
{
  OeSequence *sequence;
  GHashTable *paths; /* media_ref → owned path */
  OeRenderSource source;
} Snapshot;

typedef struct
{
  gint64 t_us;
  gint64 frame_index;
  guint generation;
  gboolean force;
  int width;
  int height;
} Request;

typedef struct
{
  OeRenderWorker *worker;
  OeRenderWorkerFrame frame;
  GError *error;
} Delivery;

struct _OeRenderWorker
{
  GThread *thread;
  GMutex lock;
  GCond cond;

  /* Mailboxes, guarded by @lock. */
  Snapshot *pending_snapshot;
  Request pending_request;
  gboolean has_request;
  gboolean quit;

  OeRenderWorkerFunc func; /* invoked on the main context only */
  gpointer user_data;

  /* Worker-thread state. */
  Snapshot *snapshot;
  OeRenderSession *render;
};

static gchar *
snapshot_resolve_path (guint media_ref, gpointer user_data)
{
  const Snapshot *snap = user_data;
  const gchar *path = g_hash_table_lookup (snap->paths, GUINT_TO_POINTER (media_ref));

  return g_strdup (path);
}

static void
snapshot_free (Snapshot *snap)
{
  if (snap == NULL)
    return;

  if (snap->sequence != NULL)
    {
      oe_sequence_clear (snap->sequence);
      g_free (snap->sequence);
    }

  g_clear_pointer (&snap->paths, g_hash_table_unref);
  g_free (snap);
}

static gboolean
deliver_on_main (gpointer data)
{
  Delivery *d = data;

  d->worker->func (&d->frame, d->error, d->worker->user_data);

  g_clear_pointer (&d->frame.rgba, g_free); /* unless the receiver took it */
  g_clear_error (&d->error);
  g_free (d);
  return G_SOURCE_REMOVE;
}

/* Worker thread: install the newest snapshot, render the newest
 * request, deliver, repeat. */
static gpointer
worker_thread (gpointer data)
{
  OeRenderWorker *worker = data;

  for (;;)
    {
      g_mutex_lock (&worker->lock);

      while (!worker->quit && worker->pending_snapshot == NULL && !worker->has_request)
        g_cond_wait (&worker->cond, &worker->lock);

      if (worker->quit)
        {
          g_mutex_unlock (&worker->lock);
          break;
        }

      Snapshot *incoming = worker->pending_snapshot;
      Request req = worker->pending_request;
      const gboolean have_request = worker->has_request;

      worker->pending_snapshot = NULL;
      worker->has_request = FALSE;
      g_mutex_unlock (&worker->lock);

      if (incoming != NULL)
        {
          /* Rebind keeps decoders for unchanged media; the old snapshot
           * must outlive the rebind (the session reads its old source
           * only through the sources it already opened, but keep the
           * order strict anyway). */
          if (worker->render != NULL)
            oe_render_session_rebind (worker->render, &incoming->source);

          snapshot_free (worker->snapshot);
          worker->snapshot = incoming;
        }

      if (!have_request)
        continue;

      Delivery *d = g_new0 (Delivery, 1);

      d->worker = worker;
      d->frame.generation = req.generation;
      d->frame.t_us = req.t_us;
      d->frame.frame_index = req.frame_index;
      d->frame.force = req.force;
      d->frame.width = req.width;
      d->frame.height = req.height;

      if (worker->snapshot == NULL)
        {
          d->error = g_error_new (OE_RENDER_ERROR, OE_RENDER_ERROR_OPEN_FAILED,
                                  "no sequence snapshot to render");
        }
      else
        {
          if (worker->render == NULL)
            worker->render = oe_render_session_new (&worker->snapshot->source);

          d->frame.rgba = oe_render_session_frame_at (worker->render, req.t_us, req.width,
                                                      req.height, &d->error);
        }

      /* Queued, never invoked directly: see the audio worker's note. */
      g_idle_add_full (G_PRIORITY_DEFAULT, deliver_on_main, d, NULL);
    }

  /* Everything FFmpeg-shaped dies on this thread. */
  g_clear_pointer (&worker->render, oe_render_session_free);
  g_clear_pointer (&worker->snapshot, snapshot_free);
  return NULL;
}

OeRenderWorker *
oe_render_worker_new (OeRenderWorkerFunc func, gpointer user_data)
{
  g_return_val_if_fail (func != NULL, NULL);

  OeRenderWorker *worker = g_new0 (OeRenderWorker, 1);

  worker->func = func;
  worker->user_data = user_data;
  g_mutex_init (&worker->lock);
  g_cond_init (&worker->cond);

  worker->thread = g_thread_new ("oe-render", worker_thread, worker);
  oe_log (OE_LOG_LEVEL_DEBUG, "render worker started");
  return worker;
}

void
oe_render_worker_free (OeRenderWorker *worker)
{
  if (worker == NULL)
    return;

  g_mutex_lock (&worker->lock);
  worker->quit = TRUE;
  g_cond_signal (&worker->cond);
  g_mutex_unlock (&worker->lock);

  g_thread_join (worker->thread);

  /* Deliveries already invoked onto the main context run now, while
   * the owner's user_data is still valid. */
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_clear_pointer (&worker->pending_snapshot, snapshot_free);
  g_cond_clear (&worker->cond);
  g_mutex_clear (&worker->lock);
  g_free (worker);
  oe_log (OE_LOG_LEVEL_DEBUG, "render worker stopped");
}

void
oe_render_worker_set_sequence (OeRenderWorker *worker, OeSequence *sequence, GHashTable *paths)
{
  g_return_if_fail (worker != NULL);
  g_return_if_fail (sequence != NULL);
  g_return_if_fail (paths != NULL);

  Snapshot *snap = g_new0 (Snapshot, 1);

  snap->sequence = sequence;
  snap->paths = paths;
  snap->source.sequence = sequence;
  snap->source.resolve_path = snapshot_resolve_path;
  snap->source.resolve_data = snap;

  g_mutex_lock (&worker->lock);

  Snapshot *superseded = worker->pending_snapshot;

  worker->pending_snapshot = snap;
  g_cond_signal (&worker->cond);
  g_mutex_unlock (&worker->lock);

  snapshot_free (superseded); /* never installed: no FFmpeg state inside */
}

void
oe_render_worker_request (OeRenderWorker *worker, gint64 t_us, gint64 frame_index, guint generation,
                          gboolean force, int width, int height)
{
  g_return_if_fail (worker != NULL);
  g_return_if_fail (width > 0 && height > 0);

  g_mutex_lock (&worker->lock);
  worker->pending_request.t_us = t_us;
  worker->pending_request.frame_index = frame_index;
  worker->pending_request.generation = generation;
  worker->pending_request.force = force;
  worker->pending_request.width = width;
  worker->pending_request.height = height;
  worker->has_request = TRUE;
  g_cond_signal (&worker->cond);
  g_mutex_unlock (&worker->lock);
}
