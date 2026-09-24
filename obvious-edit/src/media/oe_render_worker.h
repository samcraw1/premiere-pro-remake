/* oe_render_worker.h — the off-main-thread frame renderer for the
 * program monitor.
 *
 * The playback session used to render every monitor frame inline in
 * its tick, on the GTK main thread: a full decode, a swscale, and (for
 * transformed clips) the integer compositor, once per frame interval.
 * One slow frame — a 1080p H.264 picture, a seek that lands on a
 * keyframe far from the target, a rotated layer — stalled the
 * transport past its deadline, starved the audio worker's deliveries
 * (they ride the same main context), and froze the window. The Phase 5
 * notes rejected exactly this and the Phase 8 render seam reintroduced
 * it.
 *
 * This worker owns one render session on its own GThread and answers
 * "latest wins" requests: a request that arrives while one is in
 * flight replaces any still-pending one, so the worker never falls
 * more than one frame behind the transport. Frames are delivered onto
 * the main context as queued idle sources (the audio worker's pattern),
 * so the receiver runs lock-free on the main thread.
 *
 * The worker owns its sequence snapshot. The session hands over a deep
 * copy plus a media-reference → path table resolved on the main thread
 * (the project is never touched from the worker), so a refresh on the
 * main thread can never race a decode against a freed snapshot. A new
 * snapshot rebinds the render session in place: decoders for unchanged
 * media stay open across seeks and paused repaints.
 *
 * GTK never appears here. GLib, FFmpeg (through oe_render) only.
 */

#pragma once

#include <glib.h>

#include "../core/oe_project.h"

G_BEGIN_DECLS

typedef struct _OeRenderWorker OeRenderWorker;

/**
 * OeRenderWorkerFrame: one delivered frame.
 * @generation: the token the request carried — the receiver drops
 *     frames from a superseded run (a seek, a restart, a re-snapshot)
 * @t_us: the sequence time the frame was rendered at
 * @frame_index: the request's frame index (echoed for dedup)
 * @force: the request's force flag (echoed: a paused repaint)
 * @width: canvas width in pixels
 * @height: canvas height in pixels
 * @rgba: (transfer full): the owned B8G8R8A8 canvas, or NULL when the
 *     render failed. The receiver may take ownership by setting the
 *     member to NULL; whatever remains is freed after the callback.
 */
typedef struct
{
  guint generation;
  gint64 t_us;
  gint64 frame_index;
  gboolean force;
  int width;
  int height;
  guint8 *rgba;
} OeRenderWorkerFrame;

/**
 * OeRenderWorkerFunc: invoked on the main context for every completed
 * request, in request order. @error is set (and @frame->rgba is NULL)
 * when the render failed; both are borrowed.
 */
typedef void (*OeRenderWorkerFunc) (OeRenderWorkerFrame *frame, const GError *error,
                                    gpointer user_data);

/**
 * oe_render_worker_new:
 * @func: the main-context delivery callback
 * @user_data: passed to @func
 *
 * Starts the worker thread. It idles until a sequence arrives.
 *
 * Returns: (transfer full): the worker; free with
 *     oe_render_worker_free() before the media subsystem shuts down.
 */
OeRenderWorker *oe_render_worker_new (OeRenderWorkerFunc func, gpointer user_data);

/**
 * oe_render_worker_free:
 * @worker: (transfer full): the worker, or NULL (a no-op)
 *
 * Stops and joins the thread (an in-flight render completes first),
 * closes the render session on the worker thread, then pumps the main
 * context so deliveries already invoked reach @func while the owner's
 * user_data is still valid. Main thread only.
 */
void oe_render_worker_free (OeRenderWorker *worker);

/**
 * oe_render_worker_set_sequence:
 * @worker: the worker
 * @sequence: (transfer full): a heap-allocated deep copy of the sequence
 * @paths: (transfer full): media reference → owned absolute path, for
 *     every reference the sequence's clips carry (GUINT_TO_POINTER keys,
 *     g_free'd string values)
 *
 * Replaces the worker's snapshot. The swap happens on the worker thread
 * before its next render; a snapshot superseded before the worker
 * picked it up is freed unused. Cached decoders survive the swap when
 * their reference still resolves to the same path.
 */
void oe_render_worker_set_sequence (OeRenderWorker *worker, OeSequence *sequence,
                                    GHashTable *paths);

/**
 * oe_render_worker_request:
 * @worker: the worker
 * @t_us: sequence time to render
 * @frame_index: echoed in the delivery
 * @generation: echoed in the delivery
 * @force: echoed in the delivery
 * @width: canvas width (> 0)
 * @height: canvas height (> 0)
 *
 * Queues a render, replacing any request still pending (latest wins).
 * Exactly one delivery follows per request the worker actually picks
 * up; a replaced request delivers nothing.
 */
void oe_render_worker_request (OeRenderWorker *worker, gint64 t_us, gint64 frame_index,
                               guint generation, gboolean force, int width, int height);

G_END_DECLS
