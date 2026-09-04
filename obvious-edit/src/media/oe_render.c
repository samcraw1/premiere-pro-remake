/* oe_render.c — GTK-free frame-at-time render seam (Phase 8).
 *
 * One decoder per source path, kept open across frames, reading
 * forward for the export loop's strictly increasing times; backward
 * requests fall back to the decode-at seek discipline, and requests
 * past a source's last frame hold that last frame (stills render at
 * every covered sequence time this way, matching the monitor).
 * Compositing reuses oe_playback_session_map — the model's only
 * defined clip→source mapping — so a straight cut renders identically
 * here and in the program monitor.
 *
 * The decode idioms (Decoder plumbing, packet pump, box-fit swscale)
 * mirror oe_media_playback.c, which mirrors oe_media_jobs.c. FFmpeg
 * identifiers never leave this file (adapter leak rule, oe_ffmpeg.h).
 */

#include "oe_render.h"

#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include "../app/oe_playback_session.h"

GQuark
oe_render_error_quark (void)
{
  return g_quark_from_static_string ("oe-render-error");
}

/* ------------------------------------------------------------------ */
/* Shared decode plumbing (idiom from oe_media_playback.c)             */
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

/* Opens the container and the first usable video stream. */
static gboolean
decoder_open (const gchar *path, Decoder *d, GError **error)
{
  memset (d, 0, sizeof (*d));
  d->stream_index = -1;

  if (avformat_open_input (&d->fmt, path, NULL, NULL) != 0)
    {
      g_set_error (error, OE_RENDER_ERROR, OE_RENDER_ERROR_OPEN_FAILED,
                   "'%s': cannot open file", path);
      return FALSE;
    }

  if (avformat_find_stream_info (d->fmt, NULL) < 0)
    {
      g_set_error (error, OE_RENDER_ERROR, OE_RENDER_ERROR_OPEN_FAILED,
                   "'%s': cannot probe streams", path);
      decoder_close (d);
      return FALSE;
    }

  for (unsigned i = 0; i < d->fmt->nb_streams; i++)
    {
      AVStream *st = d->fmt->streams[i];

      if (st->codecpar == NULL || st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
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
      g_set_error (error, OE_RENDER_ERROR, OE_RENDER_ERROR_OPEN_FAILED,
                   "'%s': no decodable video stream", path);
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

/* First presentation timestamp of @frame in stream time_base units,
 * or 0 when the container carries none (single-frame sources). */
static gint64
frame_pts (const AVFrame *frame)
{
  if (frame->pts != AV_NOPTS_VALUE)
    return frame->pts;

  if (frame->pkt_dts != AV_NOPTS_VALUE)
    return frame->pkt_dts;

  return 0;
}

/* Decodes the next video frame in presentation order — no time filter
 * here; callers apply their own policy. Sets *end_of_stream (without
 * touching @error) when the stream is exhausted. */
static gboolean
read_next_frame (Decoder *d, AVFrame *out, gboolean *end_of_stream, GError **error)
{
  gboolean draining = FALSE;

  *end_of_stream = FALSE;

  for (;;)
    {
      int rc = avcodec_receive_frame (d->ctx, out);

      if (rc == 0)
        return TRUE;

      if (rc == AVERROR_EOF)
        {
          *end_of_stream = TRUE;
          return FALSE;
        }

      if (rc != AVERROR (EAGAIN))
        {
          g_set_error (error, OE_RENDER_ERROR, OE_RENDER_ERROR_DECODE_FAILED,
                       "video frame decode failed");
          return FALSE;
        }

      /* EAGAIN: feed more input, or drain at end of stream. */
      if (!draining)
        {
          AVPacket *pkt = av_packet_alloc ();

          g_assert_nonnull (pkt);

          if (av_read_frame (d->fmt, pkt) == 0)
            {
              if (pkt->stream_index == d->stream_index)
                send_packet (d->ctx, pkt);
              else
                av_packet_unref (pkt);

              av_packet_free (&pkt);
              continue;
            }

          av_packet_free (&pkt);
          avcodec_send_packet (d->ctx, NULL);
          draining = TRUE;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Per-source cache                                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
  Decoder dec;
  gchar *path;
  gint64 cursor_tb;           /* stream time_base pts decoded through */
  AVFrame *held;              /* last delivered frame (past-end clamp) */
  struct SwsContext *fit_sws; /* cached native → box-fit scaler */
  guint8 *fitted;             /* box-fit BGRA staging buffer */
  gint fit_w;
  gint fit_h;
} VideoSource;

static void
video_source_free (gpointer data)
{
  VideoSource *vs = data;

  decoder_close (&vs->dec);
  if (vs->held != NULL)
    av_frame_free (&vs->held);
  if (vs->fit_sws != NULL)
    sws_freeContext (vs->fit_sws);
  g_clear_pointer (&vs->fitted, g_free);
  g_free (vs->path);
  g_free (vs);
}

static void
evenize (gint *value)
{
  *value -= *value % 2;
}

/* Box-fit size of (src_w, src_h) inside (canvas_w, canvas_h) — aspect
 * preserved, rounding and evenizing exactly like the preview's decode
 * box-fit. Upscaling allowed: the monitor renders small sources large. */
static void
box_fit_size (int src_w, int src_h, int canvas_w, int canvas_h, gint *out_w, gint *out_h)
{
  double scale = MIN ((double) canvas_w / (double) src_w, (double) canvas_h / (double) src_h);
  gint fit_w = (gint) (src_w * scale + 0.5);
  gint fit_h = (gint) (src_h * scale + 0.5);

  fit_w = CLAMP (fit_w, 1, canvas_w);
  fit_h = CLAMP (fit_h, 1, canvas_h);
  evenize (&fit_w);
  evenize (&fit_h);
  fit_w = MAX (fit_w, 1);
  fit_h = MAX (fit_h, 1);
  *out_w = fit_w;
  *out_h = fit_h;
}

/* Scales @frame into the entry's staging buffer at its box-fit size. */
static gboolean
fit_frame (VideoSource *vs, const AVFrame *frame, int canvas_w, int canvas_h, gint *out_w,
           gint *out_h, GError **error)
{
  box_fit_size (frame->width, frame->height, canvas_w, canvas_h, out_w, out_h);

  if (vs->fit_sws == NULL || vs->fit_w != *out_w || vs->fit_h != *out_h)
    {
      sws_freeContext (vs->fit_sws);
      vs->fit_sws = sws_getCachedContext (NULL, frame->width, frame->height, frame->format, *out_w,
                                         *out_h, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
      vs->fit_w = *out_w;
      vs->fit_h = *out_h;
      g_clear_pointer (&vs->fitted, g_free);

      if (vs->fit_sws == NULL)
        {
          g_set_error (error, OE_RENDER_ERROR, OE_RENDER_ERROR_DECODE_FAILED,
                       "swscale setup failed");
          return FALSE;
        }

      vs->fitted = g_malloc ((gsize) *out_w * *out_h * 4);
    }

  const guint8 *src_data[4] = { frame->data[0], frame->data[1], frame->data[2], frame->data[3] };
  const int src_stride[4] = { frame->linesize[0], frame->linesize[1], frame->linesize[2],
                              frame->linesize[3] };
  guint8 *dst_data[4] = { vs->fitted, NULL, NULL, NULL };
  int dst_stride[4] = { vs->fit_w * 4, 0, 0, 0 };

  sws_scale (vs->fit_sws, src_data, src_stride, 0, frame->height, dst_data, dst_stride);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Session API                                                         */
/* ------------------------------------------------------------------ */

struct _OeRenderSession
{
  const OeRenderSource *source;
  GHashTable *sources; /* media_ref (as pointer) → owned VideoSource */
};

OeRenderSession *
oe_render_session_new (const OeRenderSource *source)
{
  g_return_val_if_fail (source != NULL, NULL);
  g_return_val_if_fail (source->sequence != NULL, NULL);
  g_return_val_if_fail (source->resolve_path != NULL, NULL);

  OeRenderSession *session = g_new0 (OeRenderSession, 1);

  session->source = source;
  session->sources
      = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, video_source_free);
  return session;
}

void
oe_render_session_free (OeRenderSession *session)
{
  if (session == NULL)
    return;

  g_clear_pointer (&session->sources, g_hash_table_unref);
  g_free (session);
}

/* Fetches (opening on first use) the decoder entry for @media_ref. */
static VideoSource *
ensure_source (OeRenderSession *session, guint media_ref, GError **error)
{
  VideoSource *vs = g_hash_table_lookup (session->sources, GUINT_TO_POINTER (media_ref));

  if (vs != NULL)
    return vs;

  gchar *path = session->source->resolve_path (media_ref, session->source->resolve_data);

  if (path == NULL)
    {
      g_set_error (error, OE_RENDER_ERROR, OE_RENDER_ERROR_OPEN_FAILED,
                   "media reference %u has no file path", media_ref);
      return NULL;
    }

  vs = g_new0 (VideoSource, 1);
  vs->path = path;

  if (!decoder_open (path, &vs->dec, error))
    {
      video_source_free (vs);
      return NULL;
    }

  g_hash_table_insert (session->sources, GUINT_TO_POINTER (media_ref), vs);
  return vs;
}

/* Fills the canvas' alpha channel: g_malloc0 leaves every byte at 0,
 * and B8G8R8A8 needs opaque pixels (letterbox included). */
static void
make_opaque (guint8 *canvas, gsize pixel_count)
{
  for (gsize i = 3; i < pixel_count * 4; i += 4)
    canvas[i] = 0xFF;
}

/* Composites @frame into a fresh @out_w x @out_h canvas: box-fit,
 * centered, opaque black letterbox — the monitor's presentation. */
static guint8 *
compose_frame (VideoSource *vs, const AVFrame *frame, int out_w, int out_h, GError **error)
{
  gint fit_w = 0;
  gint fit_h = 0;

  if (!fit_frame (vs, frame, out_w, out_h, &fit_w, &fit_h, error))
    return NULL;

  guint8 *canvas = g_malloc0 ((gsize) out_w * out_h * 4);

  const gint x0 = (out_w - fit_w) / 2;
  const gint y0 = (out_h - fit_h) / 2;

  for (gint y = 0; y < fit_h; y++)
    {
      memcpy (canvas + ((gsize) (y0 + y) * out_w + x0) * 4, vs->fitted + (gsize) y * fit_w * 4,
              (gsize) fit_w * 4);
    }

  make_opaque (canvas, (gsize) out_w * out_h);
  return canvas;
}

/* Delivers @frame: caches it as the source's held frame, updates the
 * forward cursor, composites the canvas. Takes ownership of nothing —
 * @frame is unreffed by the caller. */
static guint8 *
deliver (VideoSource *vs, const AVFrame *frame, int out_w, int out_h, GError **error)
{
  av_frame_unref (vs->held);
  av_frame_ref (vs->held, frame);
  vs->cursor_tb = frame_pts (frame);

  return compose_frame (vs, frame, out_w, out_h, error);
}

/* Renders @t_us into a fresh canvas of @out_w x @out_h. */
guint8 *
oe_render_session_frame_at (OeRenderSession *session, gint64 t_us, int out_w, int out_h,
                            GError **error)
{
  g_return_val_if_fail (session != NULL, NULL);
  g_return_val_if_fail (out_w > 0 && out_h > 0, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  OePlaybackMapping map;

  oe_playback_session_map (session->source->sequence, OE_TRACK_VIDEO, t_us, &map);

  /* No video clip covers the position: opaque black, like the monitor. */
  if (!map.active)
    {
      guint8 *canvas = g_malloc0 ((gsize) out_w * out_h * 4);

      make_opaque (canvas, (gsize) out_w * out_h);
      return canvas;
    }

  const OeTrack *track = g_ptr_array_index (session->source->sequence->tracks, map.track_index);
  const OeClip *clip = g_ptr_array_index (track->clips, map.clip_index);

  VideoSource *vs = ensure_source (session, clip->media_ref, error);

  if (vs == NULL)
    return NULL;

  /* Stream-time target for the mapped source position. */
  const gint64 target_tb = av_rescale_q (map.source_us, AV_TIME_BASE_Q, vs->dec.stream->time_base);

  if (target_tb < vs->cursor_tb)
    {
      /* Backward request: the decode-at discipline — seek backward,
       * flush, deliver the first decoded frame (preview parity). */
      if (av_seek_frame (vs->dec.fmt, vs->dec.stream_index, target_tb, AVSEEK_FLAG_BACKWARD) >= 0)
        avcodec_flush_buffers (vs->dec.ctx);
      else if (av_seek_frame (vs->dec.fmt, vs->dec.stream_index, 0, AVSEEK_FLAG_BACKWARD) >= 0)
        avcodec_flush_buffers (vs->dec.ctx);

      AVFrame *frame = av_frame_alloc ();

      g_assert_nonnull (frame);

      gboolean end_of_stream = FALSE;

      if (read_next_frame (&vs->dec, frame, &end_of_stream, error))
        {
          guint8 *canvas = deliver (vs, frame, out_w, out_h, error);

          av_frame_free (&frame);
          return canvas;
        }

      av_frame_free (&frame);

      if (end_of_stream && vs->held != NULL && vs->held->width > 0)
        return compose_frame (vs, vs->held, out_w, out_h, error);

      g_prefix_error (error, "'%s': ", vs->path);
      return NULL;
    }

  /* Forward request: read packets sequentially — no seek, the export
   * loop's hot path — dropping frames before the target. */
  for (;;)
    {
      AVFrame *frame = av_frame_alloc ();

      g_assert_nonnull (frame);

      gboolean end_of_stream = FALSE;

      if (read_next_frame (&vs->dec, frame, &end_of_stream, error))
        {
          if (frame_pts (frame) >= target_tb)
            {
              guint8 *canvas = deliver (vs, frame, out_w, out_h, error);

              av_frame_free (&frame);

              if (canvas == NULL)
                g_prefix_error (error, "'%s': ", vs->path);
              return canvas;
            }

          av_frame_free (&frame); /* still before the target */
          continue;
        }

      av_frame_free (&frame);

      if (end_of_stream)
        {
          /* Past the source's last frame: hold it (stills render at
           * every covered sequence time this way, matching the
           * monitor's clamped decode). */
          if (vs->held != NULL && vs->held->width > 0)
            return compose_frame (vs, vs->held, out_w, out_h, error);

          g_set_error (error, OE_RENDER_ERROR, OE_RENDER_ERROR_DECODE_FAILED,
                       "'%s': no video frame decoded", vs->path);
          return NULL;
        }

      g_prefix_error (error, "'%s': ", vs->path);
      return NULL;
    }
}

guint8 *
oe_render_frame_at (const OeRenderSource *source, gint64 t_us, int out_w, int out_h, GError **error)
{
  g_return_val_if_fail (source != NULL, NULL);

  OeRenderSession *session = oe_render_session_new (source);

  if (session == NULL)
    return NULL;

  guint8 *canvas = oe_render_session_frame_at (session, t_us, out_w, out_h, error);

  oe_render_session_free (session);
  return canvas;
}
