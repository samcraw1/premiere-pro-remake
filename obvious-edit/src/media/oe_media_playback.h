/* oe_media_playback.h — full-resolution playback decode capability (Phase 5).
 *
 * Two GTK-free, main-context-facing capabilities built on the FFmpeg
 * adapter idioms from oe_media_jobs.c:
 *
 *   - an audio decode-ahead worker: a GThread + GAsyncQueue worker that
 *     decodes a requested source range chunk-by-chunk into owned
 *     interleaved-float buffers and delivers them on the main context
 *     (the same invoke pattern as oe_import_worker.c);
 *   - frame-at-time video decode: seek to a source timestamp and return
 *     the frame at or after it as an owned RGBA buffer scaled into the
 *     program-monitor box.
 *
 * The worker drains and joins BEFORE oe_ffmpeg_shutdown runs — the owner
 * (the playback session) frees it inside its own free function, and the
 * session is freed before the media subsystem tears down (window dispose
 * order). Decoder internals never leak past this header.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * OePlaybackAudioChunk: one decoded block of interleaved f32 samples.
 * @source_us: source time of the first sample in the chunk
 * @sample_rate: output rate in Hz (what the caller requested)
 * @channels: output channels (interleaved)
 * @n_frames: sample frames; @interleaved holds n_frames * channels floats
 * @generation: echoes the generation the owning request carried — callers
 *     invalidate chunks from superseded requests by comparing against
 *     their own current generation
 * @interleaved: the samples, owned; freed with the chunk
 */
typedef struct
{
  gint64 source_us;
  int sample_rate;
  int channels;
  gsize n_frames;
  guint generation;
  float *interleaved;
} OePlaybackAudioChunk;

void oe_playback_audio_chunk_free (OePlaybackAudioChunk *chunk);

/**
 * OePlaybackVideoFrame: one decoded video frame, packed 8-bit RGBA,
 * box-fitted.
 * @source_us: the source time that was requested (the returned frame is
 *     the first one at or after it)
 * @width, @height: pixel dimensions of @rgba
 * @rgba: width * height * 4 bytes in B8G8R8A8 byte order (cairo ARGB32
 *     on little-endian; GDK_MEMORY_B8G8R8A8), owned; freed with the frame
 */
typedef struct
{
  gint64 source_us;
  int width;
  int height;
  guchar *rgba;
} OePlaybackVideoFrame;

void oe_playback_video_frame_free (OePlaybackVideoFrame *frame);

/**
 * OePlaybackAudioFunc: delivery callback for worker output, invoked on
 * the main context.
 *
 * @chunk != NULL: a decoded chunk (ownership transfers to the callback).
 * @chunk == NULL && @error == NULL: the requested range is exhausted.
 * @chunk == NULL && @error != NULL: the range failed to decode (missing
 *     media, unsupported stream); @error is owned by the delivery and
 *     valid only during the callback.
 * @generation: the request token every delivery echoes — chunks carry it
 *     in #OePlaybackAudioChunk, and the NULL-chunk signals carry the
 *     owning request's token so a caller can drop stale END-OF-RANGE and
 *     failure signals exactly like stale chunks (Phase 10 Wave B: the
 *     multi-track mixer chains requests, so a late signal from a
 *     superseded decode must never advance the new chain).
 */
typedef void (*OePlaybackAudioFunc) (OePlaybackAudioChunk *chunk, const GError *error,
                                     guint generation, gpointer user_data);

typedef struct _OeMediaPlaybackWorker OeMediaPlaybackWorker;

/**
 * oe_media_playback_worker_new:
 * @on_audio: delivery callback, invoked on the main context. The chunk
 *      and error stay owned by the delivery: the callback may read them
 *      for the duration of the call but must not free them.
 * @user_data: passed to @on_audio
 *
 * The worker thread starts immediately and blocks until the first request.
 *
 * Returns: (transfer full): the worker; free with
 *     oe_media_playback_worker_free() BEFORE oe_ffmpeg_shutdown().
 */
OeMediaPlaybackWorker *oe_media_playback_worker_new (OePlaybackAudioFunc on_audio,
                                                     gpointer user_data);

/**
 * oe_media_playback_worker_free:
 * @worker: (transfer full): the worker, or NULL (a no-op)
 *
 * Drains pending requests (sentinel), cancels the in-flight decode, joins
 * the thread, then flushes deliveries already queued on the main context
 * so the owner's user_data is never referenced after free. Call on the
 * main thread.
 */
void oe_media_playback_worker_free (OeMediaPlaybackWorker *worker);

/**
 * oe_media_playback_worker_request:
 * @worker: the worker
 * @path: media file path (borrowed for the call; copied internally)
 * @source_start_us, @source_end_us: half-open source range to decode
 * @sample_rate, @channels: output format (interleaved f32)
 * @generation: caller-managed token echoed on every delivered chunk
 *
 * Replaces any pending (not yet started) request and makes the worker
 * abandon a superseded in-flight decode at the next chunk boundary.
 * A seek within the same file reuses the open decoder and issues
 * avcodec_flush_buffers instead of reopening.
 */
void oe_media_playback_worker_request (OeMediaPlaybackWorker *worker, const gchar *path,
                                       gint64 source_start_us, gint64 source_end_us,
                                       int sample_rate, int channels, guint generation);

/**
 * oe_media_playback_worker_cancel:
 * @worker: the worker
 *
 * Queues a cancellation that makes the worker abandon any in-flight
 * decode at the next chunk boundary and go idle.
 */
void oe_media_playback_worker_cancel (OeMediaPlaybackWorker *worker);

/**
 * OeMediaPlaybackError: error domain for decode failures.
 * @OE_MEDIA_PLAYBACK_ERROR_OPEN_FAILED: the file could not be opened or
 *     contains no decodable stream of the requested kind.
 * @OE_MEDIA_PLAYBACK_ERROR_DECODE_FAILED: decoding failed mid-stream.
 */
typedef enum
{
  OE_MEDIA_PLAYBACK_ERROR_OPEN_FAILED,
  OE_MEDIA_PLAYBACK_ERROR_DECODE_FAILED,
} OeMediaPlaybackError;

#define OE_MEDIA_PLAYBACK_ERROR (oe_media_playback_error_quark ())

GQuark oe_media_playback_error_quark (void);

typedef struct _OeMediaVideoDecoder OeMediaVideoDecoder;

/**
 * oe_media_playback_video_open:
 * @path: media file path containing a video stream
 * @error: return location for a #GError, or NULL to ignore
 *
 * Returns: (transfer full): a decoder, or NULL with @error set.
 */
OeMediaVideoDecoder *oe_media_playback_video_open (const gchar *path, GError **error);

/**
 * oe_media_playback_video_decode_at:
 * @decoder: an open decoder
 * @source_us: source time; the frame at or after this time is returned
 * @box_w, @box_h: bounding box; the frame is scaled (aspect preserved,
 *     upscaling allowed) to fit inside it
 * @out: receives an owned OePlaybackVideoFrame
 * @error: return location for a #GError, or NULL to ignore
 *
 * Seek discipline: every call seeks backward to @source_us and issues
 * avcodec_flush_buffers, so repeated calls are independent (no residual
 * decoder state between frames). Still media (single-frame files) decode
 * their only frame for any requested time.
 *
 * Returns: TRUE with *@out set, or FALSE with @error set.
 */
gboolean oe_media_playback_video_decode_at (OeMediaVideoDecoder *decoder, gint64 source_us,
                                            int box_w, int box_h, OePlaybackVideoFrame **out,
                                            GError **error);

/**
 * oe_media_playback_video_free:
 * @decoder: (transfer full): the decoder, or NULL (a no-op)
 */
void oe_media_playback_video_free (OeMediaVideoDecoder *decoder);

G_END_DECLS
