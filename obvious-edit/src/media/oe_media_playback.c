/* oe_media_playback.c — playback decode capability implementation (Phase 5).
 *
 * Audio decode-ahead worker: one GThread fed by a GAsyncQueue, delivering
 * owned interleaved-f32 chunks onto the main context via
 * g_main_context_invoke — the same threading pattern as oe_import_worker.c.
 * Video decode: seek + flush, decode the frame at or after the requested
 * time, swscale it into an owned box-fit buffer in B8G8R8A8 byte order
 * (the packed-RGBA layout cairo surfaces consume directly).
 *
 * The decode idioms (Decoder plumbing, packet pump, box-fit swscale)
 * mirror oe_media_jobs.c. FFmpeg identifiers never leave this file
 * (adapter leak rule, oe_ffmpeg.h).
 */

#include "oe_media_playback.h"

#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "../app/oe_log.h"

GQuark
oe_media_playback_error_quark (void)
{
  return g_quark_from_static_string ("oe-media-playback-error");
}

void
oe_playback_audio_chunk_free (OePlaybackAudioChunk *chunk)
{
  if (chunk == NULL)
    return;

  g_clear_pointer (&chunk->interleaved, g_free);
  g_free (chunk);
}

void
oe_playback_video_frame_free (OePlaybackVideoFrame *frame)
{
  if (frame == NULL)
    return;

  g_clear_pointer (&frame->rgba, g_free);
  g_free (frame);
}

/* ------------------------------------------------------------------ */
/* Shared decode plumbing (idioms from oe_media_jobs.c)                */
/* ------------------------------------------------------------------ */

typedef struct
{
  AVFormatContext *fmt;
  AVCodecContext *ctx;
  AVStream *stream;
  int stream_index;
} Decoder;

static void
decoder_close (Decoder *d)
{
  if (d->ctx != NULL)
    avcodec_free_context (&d->ctx);

  if (d->fmt != NULL)
    avformat_close_input (&d->fmt);

  d->stream = NULL;
  d->stream_index = -1;
}

/* Opens the container and the first usable stream of @media_type. */
static gboolean
decoder_open (const gchar *path, enum AVMediaType media_type, Decoder *d, GError **error)
{
  memset (d, 0, sizeof (*d));
  d->stream_index = -1;

  if (avformat_open_input (&d->fmt, path, NULL, NULL) != 0)
    {
      g_set_error (error, OE_MEDIA_PLAYBACK_ERROR, OE_MEDIA_PLAYBACK_ERROR_OPEN_FAILED,
                   "'%s': cannot open file", path);
      return FALSE;
    }

  if (avformat_find_stream_info (d->fmt, NULL) < 0)
    {
      g_set_error (error, OE_MEDIA_PLAYBACK_ERROR, OE_MEDIA_PLAYBACK_ERROR_OPEN_FAILED,
                   "'%s': cannot probe streams", path);
      decoder_close (d);
      return FALSE;
    }

  for (unsigned i = 0; i < d->fmt->nb_streams; i++)
    {
      AVStream *st = d->fmt->streams[i];

      if (st->codecpar == NULL || st->codecpar->codec_type != media_type)
        continue;

      if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0)
        continue;

      const AVCodec *dec = avcodec_find_decoder (st->codecpar->codec_id);

      if (dec == NULL)
        continue;

      d->ctx = avcodec_alloc_context3 (dec);

      if (d->ctx == NULL)
        break;

      if (avcodec_parameters_to_context (d->ctx, st->codecpar) < 0
          || avcodec_open2 (d->ctx, dec, NULL) < 0)
        {
          avcodec_free_context (&d->ctx);
          d->ctx = NULL;
          continue;
        }

      d->stream = st;
      d->stream_index = (int) i;
      break;
    }

  if (d->stream_index < 0)
    {
      g_set_error (error, OE_MEDIA_PLAYBACK_ERROR, OE_MEDIA_PLAYBACK_ERROR_OPEN_FAILED,
                   "'%s': no decodable %s stream", path,
                   media_type == AVMEDIA_TYPE_AUDIO ? "audio" : "video");
      decoder_close (d);
      return FALSE;
    }

  return TRUE;
}

/* Sends one packet and consumes it either way. */
static void
send_packet (AVCodecContext *ctx, AVPacket *pkt)
{
  avcodec_send_packet (ctx, pkt);
  av_packet_unref (pkt);
}

/* ------------------------------------------------------------------ */
/* Audio decode-ahead worker                                           */
/* ------------------------------------------------------------------ */

/* Frames per delivered chunk: ~23 ms at 44.1 kHz — small enough for a
 * smooth queue, large enough that per-chunk overhead is noise. */
#define CHUNK_FRAMES 1024

typedef enum
{
  REQUEST_DECODE,
  REQUEST_CANCEL,
  REQUEST_QUIT,
} RequestKind;

typedef struct
{
  RequestKind kind;
  gchar *path;
  gint64 source_start_us;
  gint64 source_end_us;
  int sample_rate;
  int channels;
  guint generation;
} Request;

typedef struct
{
  OeMediaPlaybackWorker *worker;
  OePlaybackAudioChunk *chunk; /* NULL for signals */
  GError *error;               /* set only when chunk == NULL and failed */
  guint generation;            /* the owning request's token; chunks echo their own */
} Delivery;

struct _OeMediaPlaybackWorker
{
  GThread *thread;
  GAsyncQueue *queue;
  OePlaybackAudioFunc on_audio; /* invoked on the main context only */
  gpointer user_data;

  /* Worker-thread state: cached decoder so a seek within the same file
   * flushes (avcodec_flush_buffers) instead of reopening. */
  gboolean have_decoder;
  Decoder dec;
  gchar *decoder_path;
};

static void
free_request (Request *req)
{
  if (req == NULL)
    return;

  g_clear_pointer (&req->path, g_free);
  g_free (req);
}

static gboolean
deliver_on_main (gpointer data)
{
  Delivery *d = data;

  d->worker->on_audio (d->chunk, d->error, d->generation, d->worker->user_data);

  if (d->chunk != NULL)
    oe_playback_audio_chunk_free (d->chunk);
  g_clear_error (&d->error);
  g_free (d);
  return G_SOURCE_REMOVE;
}

/* Hand @chunk (or a NULL-chunk signal) to the main context. Ownership of
 * chunk and error transfers to the delivery. @generation stamps the
 * delivery with the owning request's token so the receiver can drop a
 * stale END-OF-RANGE or failure signal exactly like a stale chunk —
 * Phase 10 Wave B: the mixer chains decode requests per window, so a
 * late signal from a superseded decode must never advance the new
 * chain. A chunk's own generation wins; a signal carries @generation. */
static void
deliver (OeMediaPlaybackWorker *worker, OePlaybackAudioChunk *chunk, GError *error,
         guint generation)
{
  Delivery *d = g_new0 (Delivery, 1);

  d->worker = worker;
  d->chunk = chunk;
  d->error = error;
  d->generation = chunk != NULL ? chunk->generation : generation;
  g_main_context_invoke (NULL, deliver_on_main, d);
}

/* Any queued message (newer request, cancel, quit) supersedes the decode
 * in progress: request() drains older pending ones first, so whatever is
 * queued now is newer than what we are decoding. */
static gboolean
superseded (OeMediaPlaybackWorker *worker)
{
  return g_async_queue_length (worker->queue) > 0;
}

static gboolean
ensure_audio_decoder (OeMediaPlaybackWorker *worker, const gchar *path, GError **error)
{
  if (worker->have_decoder && g_strcmp0 (worker->decoder_path, path) == 0)
    return TRUE;

  if (worker->have_decoder)
    {
      decoder_close (&worker->dec);
      worker->have_decoder = FALSE;
      g_clear_pointer (&worker->decoder_path, g_free);
    }

  if (!decoder_open (path, AVMEDIA_TYPE_AUDIO, &worker->dec, error))
    return FALSE;

  worker->have_decoder = TRUE;
  worker->decoder_path = g_strdup (path);
  return TRUE;
}

/* Decodes [source_start_us, source_end_us) of the request's file into
 * interleaved f32 chunks at the requested rate/channels and delivers each
 * chunk on the main context. Ends with a NULL-chunk, NULL-error delivery
 * (range exhausted) — unless a newer message took over, which owns the
 * signals now. */
static void
decode_audio_range (OeMediaPlaybackWorker *worker, Request *req)
{
  GError *error = NULL;

  if (!ensure_audio_decoder (worker, req->path, &error))
    {
      deliver (worker, NULL, error, req->generation);
      return;
    }

  Decoder *d = &worker->dec;
  AVStream *st = d->stream;
  AVCodecContext *ctx = d->ctx;

  /* Resample everything to the requested layout: interleaved f32 at
   * (channels, rate) — what the audio stream expects. */
  AVChannelLayout out_layout;
  av_channel_layout_default (&out_layout, req->channels);

  SwrContext *swr = NULL;
  if (swr_alloc_set_opts2 (&swr, &out_layout, AV_SAMPLE_FMT_FLT, req->sample_rate,
                           &st->codecpar->ch_layout, (enum AVSampleFormat) st->codecpar->format,
                           st->codecpar->sample_rate, 0, NULL)
          < 0
      || swr == NULL || swr_init (swr) < 0)
    {
      if (swr != NULL)
        swr_free (&swr);
      deliver (worker, NULL,
               g_error_new (OE_MEDIA_PLAYBACK_ERROR, OE_MEDIA_PLAYBACK_ERROR_OPEN_FAILED,
                            "'%s': resampler setup failed", req->path),
               req->generation);
      return;
    }

  /* Seek to the range start. A reused decoder flushes here — the seek
   * discipline that keeps independent ranges independent. A failed seek
   * falls back to decoding from the start; the time filter below drops
   * the early samples. */
  if (req->source_start_us > 0)
    {
      gint64 target = av_rescale_q (req->source_start_us, AV_TIME_BASE_Q, st->time_base);

      if (av_seek_frame (d->fmt, d->stream_index, target, AVSEEK_FLAG_BACKWARD) >= 0)
        avcodec_flush_buffers (ctx);
      else if (av_seek_frame (d->fmt, d->stream_index, 0, AVSEEK_FLAG_BACKWARD) >= 0)
        avcodec_flush_buffers (ctx);
    }
  else if (av_seek_frame (d->fmt, d->stream_index, 0, AVSEEK_FLAG_BACKWARD) >= 0)
    {
      avcodec_flush_buffers (ctx);
    }

  const gint64 out_rate = req->sample_rate;
  const int out_channels = req->channels;
  const gsize chunk_bytes = CHUNK_FRAMES * (gsize) out_channels * sizeof (float);

  float *buf = g_malloc (chunk_bytes);
  gsize buf_frames = 0;
  gint64 buf_start_us = 0; /* source time of buf[0]; stamped on every empty append */
  gboolean hit_end = FALSE;

  AVPacket *pkt = av_packet_alloc ();
  AVFrame *dec_frame = av_frame_alloc ();
  g_assert_nonnull (pkt);
  g_assert_nonnull (dec_frame);

  gboolean stream_eof = FALSE;
  gboolean decoder_drained = FALSE;

  while (!hit_end && !superseded (worker))
    {
      if (!stream_eof)
        {
          if (av_read_frame (d->fmt, pkt) != 0)
            {
              stream_eof = TRUE;
              avcodec_send_packet (ctx, NULL); /* flush the decoder */
            }
          else if (pkt->stream_index == d->stream_index)
            {
              send_packet (ctx, pkt);
            }
          else
            {
              av_packet_unref (pkt);
              continue;
            }
        }
      else if (decoder_drained)
        {
          break;
        }

      gboolean got_frame = FALSE;
      while (avcodec_receive_frame (ctx, dec_frame) == 0)
        {
          got_frame = TRUE;

          if (dec_frame->nb_samples <= 0)
            continue;

          /* Convert the whole decoded frame to interleaved f32. */
          gsize temp_capacity
              = (gsize) dec_frame->nb_samples * (gsize) out_channels * sizeof (float) * 2 + 8192;
          float *temp = g_malloc (temp_capacity);
          uint8_t *dst_planes[4] = { (uint8_t *) temp, NULL, NULL, NULL };
          int n_out = swr_convert (
              swr, dst_planes, (int) (temp_capacity / ((gsize) out_channels * sizeof (float))),
              (const uint8_t **) dec_frame->extended_data, dec_frame->nb_samples);

          if (n_out <= 0)
            {
              g_free (temp);
              continue;
            }

          /* Source time of the first converted sample. */
          gint64 frame_src_us;
          if (dec_frame->pts != AV_NOPTS_VALUE)
            frame_src_us = av_rescale_q (dec_frame->pts, st->time_base, AV_TIME_BASE_Q);
          else
            frame_src_us = 0;

          /* Drop samples before the requested start (inexact seek) and
           * past the requested end. Sample i of the frame sits at
           * frame_src_us + i * 1e6 / out_rate. */
          gint64 skip = 0;
          if (frame_src_us < req->source_start_us)
            skip = (req->source_start_us - frame_src_us) * out_rate / G_GINT64_CONSTANT (1000000);

          gint64 keep_by_time = G_MAXINT64;
          if (req->source_end_us > frame_src_us)
            keep_by_time
                = (req->source_end_us - frame_src_us) * out_rate / G_GINT64_CONSTANT (1000000);

          if (skip >= n_out)
            {
              g_free (temp);
              continue;
            }

          gint64 keep = (gint64) n_out - skip;
          if (keep_by_time < keep)
            {
              keep = keep_by_time;
              hit_end = TRUE;
            }

          if (keep <= 0)
            {
              g_free (temp);
              continue;
            }

          /* Append to the chunk buffer, delivering full chunks as they
           * fill. Chunk source time advances at the output rate. The
           * buffer's anchor is stamped when the buffer is EMPTY: a full
           * buffer's first frame belongs to the sample appended there,
           * which may be a residual from an earlier decoder frame. */
          gint64 written = 0;
          while (written < keep && !superseded (worker))
            {
              gsize room = CHUNK_FRAMES - buf_frames;
              gsize take = (gsize) MIN ((gint64) room, keep - written);

              if (buf_frames == 0)
                buf_start_us = frame_src_us
                               + (skip + (gint64) written) * G_GINT64_CONSTANT (1000000) / out_rate;

              memcpy (buf + buf_frames * (gsize) out_channels,
                      temp + (size_t) (skip + written) * (gsize) out_channels,
                      take * (gsize) out_channels * sizeof (float));
              buf_frames += take;
              written += take;

              if (buf_frames == CHUNK_FRAMES)
                {
                  OePlaybackAudioChunk *chunk = g_new0 (OePlaybackAudioChunk, 1);
                  chunk->source_us = buf_start_us;
                  chunk->sample_rate = req->sample_rate;
                  chunk->channels = out_channels;
                  chunk->n_frames = buf_frames;
                  chunk->generation = req->generation;
                  chunk->interleaved = buf;
                  deliver (worker, chunk, NULL, req->generation);

                  buf = g_malloc (chunk_bytes);
                  buf_frames = 0;
                  buf_start_us += (gint64) CHUNK_FRAMES * G_GINT64_CONSTANT (1000000) / out_rate;
                }
            }

          g_free (temp);
        }

      if (stream_eof && !got_frame)
        decoder_drained = TRUE;
    }

  /* Deliver the trailing partial chunk, then the end-of-range signal —
   * unless a newer message took over, which owns the signals now. */
  if (!superseded (worker))
    {
      if (buf_frames > 0)
        {
          OePlaybackAudioChunk *chunk = g_new0 (OePlaybackAudioChunk, 1);
          chunk->source_us = buf_start_us; /* was unset: trailed as 0 and got
                                             dropped or miswritten downstream */
          chunk->sample_rate = req->sample_rate;
          chunk->channels = out_channels;
          chunk->n_frames = buf_frames;
          chunk->generation = req->generation;
          chunk->interleaved = buf;
          deliver (worker, chunk, NULL, req->generation);
          buf = NULL;
        }
      deliver (worker, NULL, NULL, req->generation);
    }

  g_free (buf);
  av_frame_free (&dec_frame);
  av_packet_free (&pkt);
  swr_free (&swr);
}

static gpointer
worker_thread (gpointer data)
{
  OeMediaPlaybackWorker *worker = data;

  for (;;)
    {
      Request *req = g_async_queue_pop (worker->queue);

      if (req->kind == REQUEST_QUIT)
        {
          free_request (req);
          break;
        }

      if (req->kind == REQUEST_DECODE)
        decode_audio_range (worker, req);

      free_request (req);
    }

  if (worker->have_decoder)
    {
      decoder_close (&worker->dec);
      worker->have_decoder = FALSE;
      g_clear_pointer (&worker->decoder_path, g_free);
    }

  return NULL;
}

OeMediaPlaybackWorker *
oe_media_playback_worker_new (OePlaybackAudioFunc on_audio, gpointer user_data)
{
  g_return_val_if_fail (on_audio != NULL, NULL);

  OeMediaPlaybackWorker *worker = g_new0 (OeMediaPlaybackWorker, 1);
  worker->on_audio = on_audio;
  worker->user_data = user_data;
  worker->queue = g_async_queue_new ();

  worker->thread = g_thread_new ("oe-playback-audio", worker_thread, worker);
  oe_log (OE_LOG_LEVEL_DEBUG, "playback audio worker started");
  return worker;
}

void
oe_media_playback_worker_request (OeMediaPlaybackWorker *worker, const gchar *path,
                                  gint64 source_start_us, gint64 source_end_us, int sample_rate,
                                  int channels, guint generation)
{
  g_return_if_fail (worker != NULL);
  g_return_if_fail (path != NULL && path[0] != '\0');
  g_return_if_fail (sample_rate > 0);
  g_return_if_fail (channels > 0);

  /* A newer request supersedes anything still pending. */
  while (g_async_queue_length (worker->queue) > 0)
    free_request (g_async_queue_pop (worker->queue));

  Request *req = g_new0 (Request, 1);
  req->kind = REQUEST_DECODE;
  req->path = g_strdup (path);
  req->source_start_us = MAX (source_start_us, 0);
  req->source_end_us = MAX (source_end_us, req->source_start_us);
  req->sample_rate = sample_rate;
  req->channels = channels;
  req->generation = generation;

  g_async_queue_push (worker->queue, req);
}

void
oe_media_playback_worker_cancel (OeMediaPlaybackWorker *worker)
{
  g_return_if_fail (worker != NULL);

  while (g_async_queue_length (worker->queue) > 0)
    free_request (g_async_queue_pop (worker->queue));

  Request *req = g_new0 (Request, 1);
  req->kind = REQUEST_CANCEL;
  g_async_queue_push (worker->queue, req);
}

void
oe_media_playback_worker_free (OeMediaPlaybackWorker *worker)
{
  if (worker == NULL)
    return;

  /* Drain: the sentinel ends the loop, and the join waits for the
   * in-flight decode to notice the queue is non-empty. Called BEFORE
   * oe_ffmpeg_shutdown by the owner. */
  Request *quit = g_new0 (Request, 1);
  quit->kind = REQUEST_QUIT;
  g_async_queue_push (worker->queue, quit);
  g_thread_join (worker->thread);
  g_async_queue_unref (worker->queue);

  /* Deliver results already invoked onto the main context while the
   * owner's user_data is still valid. */
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  g_free (worker);
}

/* ------------------------------------------------------------------ */
/* Frame-at-time video decode                                          */
/* ------------------------------------------------------------------ */

struct _OeMediaVideoDecoder
{
  Decoder dec;
};

/* Even widths keep sws and row strides happy, matching the jobs idiom. */
static void
evenize (gint *value)
{
  *value -= *value % 2;
}

gboolean
oe_media_playback_video_decode_at (OeMediaVideoDecoder *decoder, gint64 source_us, int box_w,
                                   int box_h, OePlaybackVideoFrame **out, GError **error)
{
  g_return_val_if_fail (decoder != NULL, FALSE);
  g_return_val_if_fail (out != NULL && *out == NULL, FALSE);
  g_return_val_if_fail (box_w > 0 && box_h > 0, FALSE);

  Decoder *d = &decoder->dec;

  /* Seek discipline: every call seeks backward to the requested time and
   * flushes the decoder, so calls are independent. A failed or impossible
   * seek (still media, time past the last frame) decodes from the start
   * instead. */
  gboolean seeked = FALSE;
  if (source_us > 0)
    {
      gint64 target = av_rescale_q (source_us, AV_TIME_BASE_Q, d->stream->time_base);

      if (av_seek_frame (d->fmt, d->stream_index, target, AVSEEK_FLAG_BACKWARD) >= 0)
        {
          avcodec_flush_buffers (d->ctx);
          seeked = TRUE;
        }
    }
  if (!seeked && av_seek_frame (d->fmt, d->stream_index, 0, AVSEEK_FLAG_BACKWARD) >= 0)
    avcodec_flush_buffers (d->ctx);

  AVPacket *pkt = av_packet_alloc ();
  AVFrame *frame = av_frame_alloc ();
  AVFrame *kept = NULL;
  gboolean decoder_flushed = FALSE;
  g_assert_nonnull (pkt);
  g_assert_nonnull (frame);

  while (kept == NULL)
    {
      if (!decoder_flushed && av_read_frame (d->fmt, pkt) == 0)
        {
          if (pkt->stream_index == d->stream_index)
            send_packet (d->ctx, pkt);
          else
            av_packet_unref (pkt);
        }
      else if (!decoder_flushed)
        {
          decoder_flushed = TRUE;
          avcodec_send_packet (d->ctx, NULL);
        }
      else
        {
          break; /* flushed and still nothing */
        }

      while (avcodec_receive_frame (d->ctx, frame) == 0)
        {
          if (frame->width > 0 && frame->height > 0 && kept == NULL)
            {
              kept = av_frame_alloc ();

              if (av_frame_ref (kept, frame) != 0)
                av_frame_free (&kept);
            }
        }
    }

  gboolean ok = FALSE;

  if (kept == NULL)
    {
      g_set_error (error, OE_MEDIA_PLAYBACK_ERROR, OE_MEDIA_PLAYBACK_ERROR_DECODE_FAILED,
                   "no video frame decoded");
      goto out;
    }

  /* Box-fit inside (box_w, box_h), aspect preserved, upscaling allowed —
   * the monitor may be larger than the source. */
  double scale
      = MIN ((double) box_w / (double) kept->width, (double) box_h / (double) kept->height);
  gint out_w = (gint) (kept->width * scale + 0.5);
  gint out_h = (gint) (kept->height * scale + 0.5);
  out_w = CLAMP (out_w, 1, box_w);
  out_h = CLAMP (out_h, 1, box_h);
  evenize (&out_w);
  evenize (&out_h);
  out_w = MAX (out_w, 1);
  out_h = MAX (out_h, 1);

  OePlaybackVideoFrame *result = g_new0 (OePlaybackVideoFrame, 1);
  result->source_us = source_us;
  result->width = (int) out_w;
  result->height = (int) out_h;
  result->rgba = g_malloc ((gsize) out_w * out_h * 4);

  struct SwsContext *sws
      = sws_getCachedContext (NULL, kept->width, kept->height, kept->format, out_w, out_h,
                              AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
  if (sws == NULL)
    {
      oe_playback_video_frame_free (result);
      g_set_error (error, OE_MEDIA_PLAYBACK_ERROR, OE_MEDIA_PLAYBACK_ERROR_DECODE_FAILED,
                   "swscale setup failed");
      goto out;
    }

  const guint8 *src_data[4] = { kept->data[0], kept->data[1], kept->data[2], kept->data[3] };
  const int src_stride[4]
      = { kept->linesize[0], kept->linesize[1], kept->linesize[2], kept->linesize[3] };
  guint8 *dst_data[4] = { result->rgba, NULL, NULL, NULL };
  int dst_stride[4] = { out_w * 4, 0, 0, 0 };

  sws_scale (sws, src_data, src_stride, 0, kept->height, dst_data, dst_stride);
  sws_freeContext (sws);

  *out = result;
  ok = TRUE;

out:
  av_frame_free (&kept);
  av_frame_free (&frame);
  av_packet_free (&pkt);
  return ok;
}

OeMediaVideoDecoder *
oe_media_playback_video_open (const gchar *path, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (path != NULL, NULL);

  OeMediaVideoDecoder *decoder = g_new0 (OeMediaVideoDecoder, 1);

  if (!decoder_open (path, AVMEDIA_TYPE_VIDEO, &decoder->dec, error))
    {
      g_free (decoder);
      return NULL;
    }

  return decoder;
}

void
oe_media_playback_video_free (OeMediaVideoDecoder *decoder)
{
  if (decoder == NULL)
    return;

  decoder_close (&decoder->dec);
  g_free (decoder);
}
