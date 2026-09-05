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

#include <math.h>
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
      g_set_error (error, OE_RENDER_ERROR, OE_RENDER_ERROR_OPEN_FAILED, "'%s': cannot open file",
                   path);
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
  const int src_stride[4]
      = { frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->linesize[3] };
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
  session->sources = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, video_source_free);
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
  vs->held = av_frame_alloc (); /* receive target for deliver()'s av_frame_ref */

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

guint8
oe_render_blend_channel (guint8 dst_c, guint8 src_c, guint8 src_a)
{
  /* Straight src-over on integers with a rounding bias: out =
   * (src*a + dst*(255-a) + 127) / 255. No FP anywhere (spec D2). */
  return (guint8) ((src_c * src_a + dst_c * (255 - src_a) + 127) / 255);
}

/* Blends a BGRA @layer over the opaque @canvas at (@dst_x, @dst_y)
 * with a global @opacity multiplier. The layer's own alpha times
 * @opacity drives the blend; fully transparent pixels (and clipped
 * regions) leave the canvas untouched. */
static void
blend_layer (guint8 *canvas, int canvas_w, int canvas_h, const guint8 *layer, int layer_w,
             int layer_h, int dst_x, int dst_y, guint8 opacity)
{
  const gint x0 = CLAMP (dst_x, 0, canvas_w);
  const gint x1 = CLAMP (dst_x + layer_w, 0, canvas_w);
  const gint y0 = CLAMP (dst_y, 0, canvas_h);
  const gint y1 = CLAMP (dst_y + layer_h, 0, canvas_h);

  for (gint y = y0; y < y1; y++)
    {
      const guint8 *src = layer + (gsize) (y - dst_y) * layer_w * 4 + (gsize) (x0 - dst_x) * 4;
      guint8 *dst = canvas + (gsize) y * canvas_w * 4 + (gsize) x0 * 4;

      for (gint x = x0; x < x1; x++)
        {
          const guint a = src[3] * opacity / 255;

          if (a != 0)
            {
              dst[0] = oe_render_blend_channel (dst[0], src[0], (guint8) a);
              dst[1] = oe_render_blend_channel (dst[1], src[1], (guint8) a);
              dst[2] = oe_render_blend_channel (dst[2], src[2], (guint8) a);
            }

          dst += 4;
          src += 4;
        }
    }
}

/* Scales @src by @scale_permille (1000 = 1.0x), nearest-neighbor on
 * integer coordinates — deterministic, and it cannot invent values at
 * layer seams the way filtered scaling would. */
static guint8 *
scale_layer (const guint8 *src, int src_w, int src_h, guint scale_permille, int *out_w, int *out_h)
{
  const gint dw = MAX ((src_w * (gint) scale_permille + 500) / 1000, 1);
  const gint dh = MAX ((src_h * (gint) scale_permille + 500) / 1000, 1);
  guint8 *dst = g_malloc0 ((gsize) dw * dh * 4);

  for (gint y = 0; y < dh; y++)
    {
      const gint sy = MIN (y * 1000 / (gint) scale_permille, src_h - 1);

      for (gint x = 0; x < dw; x++)
        {
          const gint sx = MIN (x * 1000 / (gint) scale_permille, src_w - 1);

          memcpy (dst + ((gsize) y * dw + x) * 4, src + ((gsize) sy * src_w + sx) * 4, 4);
        }
    }

  *out_w = dw;
  *out_h = dh;
  return dst;
}

/* Rotates @src about its center by @rotation_cdeg (clockwise, 1/100
 * degree units) into a freshly allocated buffer sized to the rotated
 * bounding box. Inverse mapping: each destination pixel is pulled from
 * the source through 2^15 fixed-point trig with two-stage bilinear
 * interpolation — integer arithmetic in the pixel loop, so the output
 * is bit-identical across runs and platforms. Samples outside the
 * source stay transparent (zeroed). */
static guint8 *
rotate_layer (const guint8 *src, int src_w, int src_h, int rotation_cdeg, int *out_w, int *out_h)
{
  if (rotation_cdeg == 0)
    {
      guint8 *copy = g_memdup2 (src, (gsize) src_w * src_h * 4);

      *out_w = src_w;
      *out_h = src_h;
      return copy;
    }

  const double rad = rotation_cdeg * (G_PI / 18000.0); /* 1/100 deg → rad */
  const double cos_r = cos (rad);
  const double sin_r = sin (rad);
  const gint dw = (gint) ceil (src_w * fabs (cos_r) + src_h * fabs (sin_r));
  const gint dh = (gint) ceil (src_w * fabs (sin_r) + src_h * fabs (cos_r));
  const gint c15 = (gint) lround (cos_r * 32768.0); /* 2^15 fixed point */
  const gint s15 = (gint) lround (sin_r * 32768.0);
  const gint64 sw15 = (gint64) (src_w - 1) << 15; /* pixel ← centered map */
  const gint64 sh15 = (gint64) (src_h - 1) << 15;
  guint8 *dst = g_malloc0 ((gsize) dw * dh * 4);

  for (gint dy = 0; dy < dh; dy++)
    {
      for (gint dx = 0; dx < dw; dx++)
        {
          /* Centered destination coordinate, half-pixel units in 2^15
           * fixed point: (2*d + 1 - size) << 15. */
          const gint64 u15 = ((gint64) (2 * dx + 1 - dw)) << 15;
          const gint64 v15 = ((gint64) (2 * dy + 1 - dh)) << 15;
          /* Inverse rotation into source space (same 2^15 units). */
          const gint64 x15 = (((gint64) c15 * u15 + (gint64) s15 * v15) >> 15) + sw15;
          const gint64 y15 = (((gint64) c15 * v15 - (gint64) s15 * u15) >> 15) + sh15;
          /* Pixel coordinate x = (X2 + w - 1) / 2, still 2^15-scaled. */
          const gint64 px15 = x15 >> 1;
          const gint64 py15 = y15 >> 1;
          const gint sx = (gint) (px15 >> 15);
          const gint sy = (gint) (py15 >> 15);

          if (sx < 0 || sy < 0 || sx >= src_w - 1 || sy >= src_h - 1)
            continue; /* outside the source: destination stays transparent */

          const gint fx = (gint) ((px15 - ((px15 >> 15) << 15)) >> 7); /* 8-bit */
          const gint fy = (gint) ((py15 - ((py15 >> 15) << 15)) >> 7);

          for (gint ch = 0; ch < 4; ch++)
            {
              const gint tl = src[((gsize) sy * src_w + sx) * 4 + ch];
              const gint tr = src[((gsize) sy * src_w + sx + 1) * 4 + ch];
              const gint bl = src[((gsize) (sy + 1) * src_w + sx) * 4 + ch];
              const gint br = src[((gsize) (sy + 1) * src_w + sx + 1) * 4 + ch];
              const gint top = tl + (((tr - tl) * fx) >> 7);
              const gint bot = bl + (((br - bl) * fx) >> 7);

              dst[((gsize) dy * dw + dx) * 4 + ch] = (guint8) (top + (((bot - top) * fy) >> 7));
            }
        }
    }

  *out_w = dw;
  *out_h = dh;
  return dst;
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

/* Decode-to-frame for one source at a stream-time target: backward
 * requests seek + flush (decode-at discipline), forward requests ride
 * the sequential pump. Cursor and held frame advance exactly like
 * deliver() minus compositing, so the fast path and the layered path
 * share one decode story. Returns @vs's held frame on success, NULL
 * with @error set on failure. */
static const AVFrame *
source_frame_at (VideoSource *vs, gint64 target_tb, GError **error)
{
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
          av_frame_unref (vs->held);
          av_frame_ref (vs->held, frame);
          vs->cursor_tb = frame_pts (frame);
          av_frame_free (&frame);
          return vs->held;
        }

      av_frame_free (&frame);

      if (end_of_stream && vs->held != NULL && vs->held->width > 0)
        return vs->held;

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
              av_frame_unref (vs->held);
              av_frame_ref (vs->held, frame);
              vs->cursor_tb = frame_pts (frame);
              av_frame_free (&frame);
              return vs->held;
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
            return vs->held;

          g_set_error (error, OE_RENDER_ERROR, OE_RENDER_ERROR_DECODE_FAILED,
                       "'%s': no video frame decoded", vs->path);
          return NULL;
        }

      g_prefix_error (error, "'%s': ", vs->path);
      return NULL;
    }
}

/* Covering video clips at @t_us, ascending track order — the blend
 * order. Mirrors oe_playback_session_map's cover rule and source
 * clamp; the array holds borrowed clip pointers. A transition window
 * contributes a SECOND entry for its track (the non-covering neighbor
 * with its source clamped to the shared boundary edge), and both pair
 * members carry their ramp weight (255 = not part of a pair). */
typedef struct
{
  const OeClip *clip; /* NULL renders the pinned-black dip side */
  gint64 source_us;
  gint64 clip_time_us; /* clip-relative sample time for keyframes */
  guint blend_weight;
} CoveringClip;

/* The covering entry's source clamp — also the transition partner's
 * rule: the incoming neighbor freezes on its first frame before the
 * boundary, the outgoing neighbor on its last frame after it. */
static gint64
cover_source_us (const OeClip *clip, gint64 t_us)
{
  return CLAMP (clip->source_in_us + (t_us - clip->position_us), clip->source_in_us,
                clip->source_out_us - 1);
}

static void
collect_covering (const OeSequence *sequence, gint64 t_us, GArray *out)
{
  for (guint t = 0; t < sequence->tracks->len; t++)
    {
      const OeTrack *track = g_ptr_array_index (sequence->tracks, t);

      if (track->kind != OE_TRACK_VIDEO || track->clips == NULL)
        continue;

      for (guint c = 0; c < track->clips->len; c++)
        {
          const OeClip *clip = g_ptr_array_index (track->clips, c);
          const gint64 length = clip->source_out_us - clip->source_in_us;

          if (t_us < clip->position_us || t_us >= clip->position_us + length)
            continue;

          const CoveringClip entry = {
            .clip = clip,
            .source_us = cover_source_us (clip, t_us),
            .clip_time_us = CLAMP (t_us - clip->position_us, 0, length - 1),
            .blend_weight = 255,
          };

          g_array_append_val (out, entry);
        }

      /* Transition windows (D5): the effective window is re-derived
       * from the current clips every frame — a moved or trimmed
       * neighbor degrades the transition to the straight cut here, by
       * simply not contributing a pair. One active window per track
       * (v1). */
      for (guint tr = 0; tr < sequence->transitions->len; tr++)
        {
          const OeTransition *transition = g_ptr_array_index (sequence->transitions, tr);
          const OeTransitionWindow w = oe_transition_window (sequence, transition);

          if (!w.active || transition->track_index != t)
            continue;
          if (t_us < w.start_us || t_us >= w.end_us)
            continue;

          gint64 ramp = oe_time_round_ratio ((t_us - w.start_us) * 255, transition->duration_us);
          ramp = CLAMP (ramp, 0, 255);

          /* w = 0/255 are the documented degenerate points: the ramp
           * collapses to the straight cut of the covering clip, no
           * pair contribution at all. */
          if (ramp == 0 || ramp == 255)
            break;

          /* Dip-to-black runs the same ramp against a pinned-black
           * side instead of the neighbor clip (same formula, same
           * weights — only the partner's pixels differ). */
          const gboolean dip = transition->kind == OE_TRANSITION_DIP_TO_BLACK;

          if (t_us < transition->at_us)
            {
              /* Outgoing half: the covering clip is @out; the partner
               * fades in frozen on its first frame (or black). */
              CoveringClip partner = {
                .clip = dip ? NULL : w.in_clip,
                .source_us = dip ? 0 : w.in_clip->source_in_us,
                .clip_time_us = 0, /* frozen head */
                .blend_weight = (guint) ramp,
              };

              g_array_append_val (out, partner);
              g_array_index (out, CoveringClip, out->len - 2).blend_weight = 255 - (guint) ramp;
            }
          else
            {
              /* Incoming half: the covering clip is @in; the partner
               * fades out frozen on its last frame (or black). */
              const gint64 out_len = w.out_clip->source_out_us - w.out_clip->source_in_us;
              CoveringClip partner = {
                .clip = dip ? NULL : w.out_clip,
                .source_us = dip ? 0 : cover_source_us (w.out_clip, t_us),
                .clip_time_us = dip ? 0 : out_len - 1, /* frozen tail */
                .blend_weight = 255 - (guint) ramp,
              };

              g_array_append_val (out, partner);
              g_array_index (out, CoveringClip, out->len - 2).blend_weight = (guint) ramp;
            }

          break; /* one active window per track */
        }
    }
}

/* The layered path (spec D2): decode → crop → scale → rotate →
 * translate to the centered position → straight src-over blend, per
 * covering clip in ascending track order. */
static guint8 *
compose_layered (OeRenderSession *session, const GArray *covering, int out_w, int out_h,
                 GError **error)
{
  guint8 *canvas = g_malloc0 ((gsize) out_w * out_h * 4);

  make_opaque (canvas, (gsize) out_w * out_h);

  /* Transition pair state: the first member's placed canvas waits here
   * until its partner arrives (pair members are adjacent in the
   * covering array — collect_covering appends them back to back). */
  guint8 *pair_a = NULL;
  guint pair_a_weight = 0;

  for (guint i = 0; i < covering->len; i++)
    {
      const CoveringClip *entry = &g_array_index (covering, CoveringClip, i);
      /* One resolution point for preview and export alike: keyframed
       * properties sample at the entry's clip-relative time; a clip
       * without keyframes passes through unchanged. */
      OeClipVisual resolved = { 0 };
      const OeClipVisual *visual = NULL;

      if (entry->clip != NULL)
        {
          oe_clip_visual_resolve (&entry->clip->visual, entry->clip_time_us, &resolved);
          visual = &resolved;
        }
      guint8 *layer = NULL;
      int layer_w = 0;
      int layer_h = 0;

      if (visual != NULL && visual->opacity == 0)
        {
          /* Invisible layer: a solo entry blends nothing; a pair
           * member still ramps (its placed canvas is black) — handled
           * by falling through with a NULL layer only for solos. */
          if (entry->blend_weight == 255)
            continue;
        }

      if (visual != NULL)
        {
          VideoSource *vs = ensure_source (session, entry->clip->media_ref, error);

          if (vs == NULL)
            {
              g_free (canvas);
              g_free (pair_a);
              return NULL;
            }

          const gint64 target_tb
              = av_rescale_q (entry->source_us, AV_TIME_BASE_Q, vs->dec.stream->time_base);
          const AVFrame *frame = source_frame_at (vs, target_tb, error);

          if (frame == NULL)
            {
              g_free (canvas);
              g_free (pair_a);
              return NULL;
            }

          gint fit_w = 0;
          gint fit_h = 0;

          if (!fit_frame (vs, frame, out_w, out_h, &fit_w, &fit_h, error))
            {
              g_free (canvas);
              g_free (pair_a);
              return NULL;
            }

          /* Own a copy: fit_frame's staging buffer is reused per source,
           * and the pipeline below frees every intermediate. */
          layer = g_memdup2 (vs->fitted, (gsize) fit_w * fit_h * 4);
          layer_w = fit_w;
          layer_h = fit_h;

          /* Crop first (source pixels, spec D2), then scale, then rotate. */
          if (visual->crop_l != 0 || visual->crop_t != 0 || visual->crop_r != 0
              || visual->crop_b != 0)
            {
              const gint cl = MIN ((gint) visual->crop_l, layer_w / 2);
              const gint ct = MIN ((gint) visual->crop_t, layer_h / 2);
              const gint cr = MIN ((gint) visual->crop_r, layer_w / 2);
              const gint cb = MIN ((gint) visual->crop_b, layer_h / 2);
              const gint cw = MAX (layer_w - cl - cr, 1);
              const gint ch = MAX (layer_h - ct - cb, 1);
              guint8 *cropped = g_malloc0 ((gsize) cw * ch * 4);

              for (gint y = 0; y < ch; y++)
                memcpy (cropped + (gsize) y * cw * 4,
                        layer + (gsize) (ct + y) * layer_w * 4 + (gsize) cl * 4, (gsize) cw * 4);

              g_free (layer);
              layer = cropped;
              layer_w = cw;
              layer_h = ch;
            }

          if (!oe_clip_visual_is_default (visual))
            {
              int scaled_w = 0;
              int scaled_h = 0;
              guint8 *scaled = scale_layer (layer, layer_w, layer_h, visual->scale_permille,
                                            &scaled_w, &scaled_h);

              g_free (layer);

              int rotated_w = 0;
              int rotated_h = 0;
              guint8 *rotated = rotate_layer (scaled, scaled_w, scaled_h, visual->rotation_cdeg,
                                              &rotated_w, &rotated_h);

              g_free (scaled);
              layer = rotated;
              layer_w = rotated_w;
              layer_h = rotated_h;
            }
        }
      else
        {
          /* Pinned-black dip side: an opaque black frame. */
          layer = g_malloc0 ((gsize) out_w * out_h * 4);
          make_opaque (layer, (gsize) out_w * out_h);
          layer_w = out_w;
          layer_h = out_h;
        }

      /* Centered anchor, then the frame-pixel position offset. The
       * pinned-black side spans the whole canvas, so it has no offset
       * and always renders at full opacity. */
      const gint pos_x = visual != NULL ? visual->pos_x : 0;
      const gint pos_y = visual != NULL ? visual->pos_y : 0;
      const guint opacity = visual != NULL ? visual->opacity : 255;
      const gint dst_x = (out_w - layer_w) / 2 + pos_x;
      const gint dst_y = (out_h - layer_h) / 2 + pos_y;

      if (entry->blend_weight == 255)
        {
          blend_layer (canvas, out_w, out_h, layer, layer_w, layer_h, dst_x, dst_y, opacity);
          g_free (layer);
          continue;
        }

      /* Transition pair member: place on its own opaque canvas (the
       * layer's own opacity still applies), then ramp with the partner
       * once both are in hand. */
      guint8 *placed = g_malloc0 ((gsize) out_w * out_h * 4);

      make_opaque (placed, (gsize) out_w * out_h);
      blend_layer (placed, out_w, out_h, layer, layer_w, layer_h, dst_x, dst_y, opacity);
      g_free (layer);

      if (pair_a == NULL)
        {
          pair_a = placed;
          pair_a_weight = entry->blend_weight;
          continue;
        }

      /* out = (A*(255-w) + B*w)/255 per channel, truncating — the D5
       * integer ramp; weights come from collect_covering and sum to
       * 255. Alpha stays opaque: the pair IS the track's frame. */
      const guint wa = pair_a_weight;
      const guint wb = entry->blend_weight;
      const gsize pixels = (gsize) out_w * out_h * 4;

      for (gsize p = 0; p < pixels; p += 4)
        {
          canvas[p + 0] = (guint8) ((pair_a[p + 0] * wa + placed[p + 0] * wb) / 255);
          canvas[p + 1] = (guint8) ((pair_a[p + 1] * wa + placed[p + 1] * wb) / 255);
          canvas[p + 2] = (guint8) ((pair_a[p + 2] * wa + placed[p + 2] * wb) / 255);
          canvas[p + 3] = 255;
        }

      g_free (pair_a);
      pair_a = NULL;
      g_free (placed);
    }

  /* Defensive: an unpaired member (collect_covering always appends
   * both sides, so this is unreachable today) blends as-is. */
  g_free (pair_a);
  return canvas;
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

  GArray *covering = g_array_sized_new (FALSE, FALSE, sizeof (CoveringClip), 4);

  collect_covering (session->source->sequence, t_us, covering);

  guint8 *canvas = NULL;

  /* Fast path: exactly one covering clip with the default transform
   * composites through the untouched single-layer pipeline — the
   * pre-Phase-9 byte-identical presentation (straight cuts, parity). */
  if (covering->len == 1
      && oe_clip_visual_is_default (&g_array_index (covering, CoveringClip, 0).clip->visual))
    {
      const CoveringClip *entry = &g_array_index (covering, CoveringClip, 0);
      VideoSource *vs = ensure_source (session, entry->clip->media_ref, error);

      if (vs != NULL)
        {
          const gint64 target_tb
              = av_rescale_q (entry->source_us, AV_TIME_BASE_Q, vs->dec.stream->time_base);
          const AVFrame *frame = source_frame_at (vs, target_tb, error);

          if (frame != NULL)
            canvas = compose_frame (vs, frame, out_w, out_h, error);
        }
    }
  else
    canvas = compose_layered (session, covering, out_w, out_h, error);

  g_array_unref (covering);
  return canvas;
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
