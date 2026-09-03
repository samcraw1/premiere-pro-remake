/* oe_media_jobs.c — thumbnail and waveform decode implementation (Phase 2).
 *
 * Everything here runs on the import worker thread. The functions are
 * pure: path in, owned buffer out, optional cancel check between decode
 * steps. FFmpeg identifiers never leave this file (adapter leak rule,
 * oe_ffmpeg.h).
 */

#include "oe_media_jobs.h"

#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "../app/oe_log.h"

GQuark
oe_media_job_error_quark (void)
{
  return g_quark_from_static_string ("oe-media-job-error");
}

void
oe_thumbnail_free (OeThumbnail *thumb)
{
  if (thumb == NULL)
    return;

  g_clear_pointer (&thumb->rgba, g_free);
  thumb->width = 0;
  thumb->height = 0;
}

void
oe_waveform_free (OeWaveform *wf)
{
  if (wf == NULL)
    return;

  g_clear_pointer (&wf->peaks, g_free);
  wf->bucket_count = 0;
}

/* Seek target: 10% of the duration, capped at 3 s (integer arithmetic
 * end to end — no float timestamps). */
static gint64
seek_target_us (gint64 duration_us)
{
  if (duration_us <= 0)
    return 0;

  return MIN (duration_us / 10, G_GINT64_CONSTANT (3000000));
}

static gboolean
cancelled (OeMediaJobCancel cancel, gpointer cancel_data)
{
  return cancel != NULL && cancel (cancel_data);
}

static void
set_job_error (GError **error, OeMediaJobError code, const gchar *path, const gchar *detail)
{
  g_set_error (error, OE_MEDIA_JOB_ERROR, code, "'%s': %s", path, detail);
}

/* ------------------------------------------------------------------ */
/* Shared decode plumbing                                              */
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
decoder_open (const gchar *path, enum AVMediaType media_type, Decoder *d, GError **error,
              OeMediaJobError fail_code, const gchar *fail_detail)
{
  memset (d, 0, sizeof (*d));
  d->stream_index = -1;

  if (avformat_open_input (&d->fmt, path, NULL, NULL) != 0)
    {
      set_job_error (error, OE_MEDIA_JOB_ERROR_OPEN_FAILED, path, "cannot open file");
      return FALSE;
    }

  if (avformat_find_stream_info (d->fmt, NULL) < 0)
    {
      set_job_error (error, fail_code, path, fail_detail);
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
      set_job_error (error, fail_code, path, fail_detail);
      decoder_close (d);
      return FALSE;
    }

  return TRUE;
}

/* Receives every frame the decoder currently owes. */
typedef void (*FrameVisitor) (AVFrame *frame, gpointer user_data);

static void
receive_frames (AVCodecContext *ctx, AVFrame *frame, FrameVisitor visit, gpointer user_data)
{
  while (avcodec_receive_frame (ctx, frame) == 0)
    visit (frame, user_data);
}

/* Sends one packet and consumes it either way. */
static void
send_packet (AVCodecContext *ctx, AVPacket *pkt)
{
  avcodec_send_packet (ctx, pkt);
  av_packet_unref (pkt);
}

/* ------------------------------------------------------------------ */
/* Thumbnail job                                                       */
/* ------------------------------------------------------------------ */

typedef struct
{
  AVFrame *kept; /* referenced frame, set by the first visit */
} FirstFrame;

static void
keep_first_frame (AVFrame *frame, gpointer user_data)
{
  FirstFrame *ff = user_data;

  if (ff->kept != NULL || frame->width <= 0 || frame->height <= 0)
    return;

  AVFrame *ref = av_frame_alloc ();

  if (ref != NULL && av_frame_ref (ref, frame) == 0)
    ff->kept = ref;
  else
    av_frame_free (&ref);
}

/* Read loop until a frame decodes or the stream ends. */
static void
read_until_frame (Decoder *d, FirstFrame *ff, AVFrame *frame, AVPacket *pkt,
                  OeMediaJobCancel cancel, gpointer cancel_data)
{
  while (!ff->kept)
    {
      if (cancelled (cancel, cancel_data) || av_read_frame (d->fmt, pkt) != 0)
        return;

      if (pkt->stream_index != d->stream_index)
        {
          av_packet_unref (pkt);
          continue;
        }

      send_packet (d->ctx, pkt);
      receive_frames (d->ctx, frame, keep_first_frame, ff);
    }
}

/* Decodes the first frame after @seek_us (0 = from the start), falling
 * back to the very first frame when the seeked position yields nothing.
 * Returns TRUE with a referenced frame in @out. */
static gboolean
decode_first_frame (Decoder *d, gint64 seek_us, AVFrame **out, OeMediaJobCancel cancel,
                    gpointer cancel_data)
{
  AVPacket *pkt = av_packet_alloc ();
  AVFrame *frame = av_frame_alloc ();
  FirstFrame ff = { NULL };

  g_assert_nonnull (pkt);
  g_assert_nonnull (frame);

  if (seek_us > 0)
    {
      gint64 target = av_rescale_q (seek_us, AV_TIME_BASE_Q, d->stream->time_base);

      if (av_seek_frame (d->fmt, d->stream_index, target, AVSEEK_FLAG_BACKWARD) >= 0)
        avcodec_flush_buffers (d->ctx);
      /* A failed seek leaves us at the start: the natural fallback. */
    }

  read_until_frame (d, &ff, frame, pkt, cancel, cancel_data);

  if (!ff.kept && !cancelled (cancel, cancel_data))
    {
      /* Drain the decoder after EOF... */
      avcodec_send_packet (d->ctx, NULL);
      receive_frames (d->ctx, frame, keep_first_frame, &ff);
    }

  if (!ff.kept && !cancelled (cancel, cancel_data) && seek_us > 0)
    {
      /* ...and retry from the very first frame when the seek landed past
       * all decodable content. */
      av_seek_frame (d->fmt, d->stream_index, INT64_MIN, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers (d->ctx);
      read_until_frame (d, &ff, frame, pkt, cancel, cancel_data);

      if (!ff.kept)
        {
          avcodec_send_packet (d->ctx, NULL);
          receive_frames (d->ctx, frame, keep_first_frame, &ff);
        }
    }

  av_packet_free (&pkt);
  av_frame_free (&frame);

  *out = ff.kept;
  return ff.kept != NULL;
}

/* Integer box fit: scale = BOX / max(w, h), rounded, at least 1 px. */
static void
box_fit (gint width, gint height, gint *out_w, gint *out_h)
{
  const gint64 box = OE_THUMBNAIL_BOX;
  const gint64 larger = MAX (width, height);

  g_assert (larger > 0);

  /* Never upscale: a small source keeps its native size, so the display
   * layer scales it without compounding interpolation artefacts. */
  if (larger <= box)
    {
      *out_w = width;
      *out_h = height;
      return;
    }

  gint64 w = (width * box + larger / 2) / larger;
  gint64 h = (height * box + larger / 2) / larger;

  *out_w = (int) MAX (w, 1);
  *out_h = (int) MAX (h, 1);
}

gboolean
oe_media_job_thumbnail (const gchar *path, OeMediaJobCancel cancel, gpointer cancel_data,
                        OeThumbnail *out, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  memset (out, 0, sizeof (*out));

  Decoder d;

  if (!decoder_open (path, AVMEDIA_TYPE_VIDEO, &d, error, OE_MEDIA_JOB_ERROR_UNSUPPORTED,
                     "no decodable video stream"))
    return FALSE;

  gint64 duration_us = 0;

  if (d.fmt->duration != AV_NOPTS_VALUE && d.fmt->duration > 0)
    duration_us = d.fmt->duration;

  if (cancelled (cancel, cancel_data))
    {
      set_job_error (error, OE_MEDIA_JOB_ERROR_CANCELLED, path, "cancelled");
      decoder_close (&d);
      return FALSE;
    }

  AVFrame *src = NULL;

  if (!decode_first_frame (&d, seek_target_us (duration_us), &src, cancel, cancel_data))
    {
      if (cancelled (cancel, cancel_data))
        set_job_error (error, OE_MEDIA_JOB_ERROR_CANCELLED, path, "cancelled");
      else
        set_job_error (error, OE_MEDIA_JOB_ERROR_UNSUPPORTED, path, "no video frame decoded");
      decoder_close (&d);
      return FALSE;
    }

  gint out_w = 0;
  gint out_h = 0;

  box_fit (src->width, src->height, &out_w, &out_h);

  guchar *rgba = g_malloc ((gsize) out_w * out_h * 4);
  struct SwsContext *sws
      = sws_getCachedContext (NULL, src->width, src->height, src->format, out_w, out_h,
                              AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);

  if (sws == NULL)
    {
      g_free (rgba);
      av_frame_free (&src);
      set_job_error (error, OE_MEDIA_JOB_ERROR_UNSUPPORTED, path, "swscale setup failed");
      decoder_close (&d);
      return FALSE;
    }

  const guint8 *src_data[4] = { src->data[0], src->data[1], src->data[2], src->data[3] };
  const int src_stride[4]
      = { src->linesize[0], src->linesize[1], src->linesize[2], src->linesize[3] };
  guint8 *dst_data[4] = { rgba, NULL, NULL, NULL };
  int dst_stride[4] = { out_w * 4, 0, 0, 0 };

  sws_scale (sws, src_data, src_stride, 0, src->height, dst_data, dst_stride);
  sws_freeContext (sws);
  av_frame_free (&src);
  decoder_close (&d);

  out->width = out_w;
  out->height = out_h;
  out->rgba = rgba;

  oe_log (OE_LOG_LEVEL_DEBUG, "thumbnail for '%s': %dx%d", path, out_w, out_h);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Waveform job                                                        */
/* ------------------------------------------------------------------ */

gboolean
oe_media_job_waveform (const gchar *path, OeMediaJobCancel cancel, gpointer cancel_data,
                       OeWaveform *out, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  memset (out, 0, sizeof (*out));

  Decoder d;

  if (!decoder_open (path, AVMEDIA_TYPE_AUDIO, &d, error, OE_MEDIA_JOB_ERROR_UNSUPPORTED,
                     "no decodable audio stream"))
    return FALSE;

  /* Convert everything to mono s16 at the source rate; only amplitude
   * matters for peaks, so no rate conversion is requested. */
  AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
  SwrContext *swr = NULL;
  int rv = swr_alloc_set_opts2 (&swr, &mono, AV_SAMPLE_FMT_S16, d.stream->codecpar->sample_rate,
                                &d.stream->codecpar->ch_layout, d.stream->codecpar->format,
                                d.stream->codecpar->sample_rate, 0, NULL);

  if (rv != 0 || swr == NULL || swr_init (swr) < 0)
    {
      if (swr != NULL)
        swr_free (&swr);
      set_job_error (error, OE_MEDIA_JOB_ERROR_UNSUPPORTED, path, "resampler setup failed");
      decoder_close (&d);
      return FALSE;
    }

  GArray *samples = g_array_new (FALSE, FALSE, sizeof (gint16));
  AVPacket *pkt = av_packet_alloc ();
  AVFrame *frame = av_frame_alloc ();

  g_assert_nonnull (pkt);
  g_assert_nonnull (frame);

  gboolean stopped = FALSE;

  while (!stopped)
    {
      if (cancelled (cancel, cancel_data))
        {
          set_job_error (error, OE_MEDIA_JOB_ERROR_CANCELLED, path, "cancelled");
          stopped = TRUE;
          break;
        }

      rv = av_read_frame (d.fmt, pkt);

      if (rv != 0)
        {
          /* EOF or read error: flush the decoder and finish. */
          av_packet_unref (pkt);
          avcodec_send_packet (d.ctx, NULL);
          stopped = TRUE;
        }
      else if (pkt->stream_index == d.stream_index)
        {
          send_packet (d.ctx, pkt);
        }
      else
        {
          av_packet_unref (pkt);
          continue;
        }

      while (avcodec_receive_frame (d.ctx, frame) == 0)
        {
          /* Convert to mono s16; swr keeps any output that does not fit
           * in the staging buffer and hands it over on later calls, in
           * order, so per-frame conversion preserves sample order. */
          gint16 staging[8192];
          guint8 *out_ptr = (guint8 *) staging;
          int got = swr_convert (swr, &out_ptr, (int) G_N_ELEMENTS (staging),
                                 (const guint8 **) frame->extended_data, frame->nb_samples);

          if (got > 0)
            g_array_append_vals (samples, staging, (guint) got);
        }
    }

  /* Drain whatever swr still buffers. */
  for (;;)
    {
      gint16 staging[8192];
      guint8 *out_ptr = (guint8 *) staging;
      int got = swr_convert (swr, &out_ptr, (int) G_N_ELEMENTS (staging), NULL, 0);

      if (got <= 0)
        break;

      g_array_append_vals (samples, staging, (guint) got);
    }

  av_packet_free (&pkt);
  av_frame_free (&frame);
  swr_free (&swr);
  decoder_close (&d);

  const gsize total = samples->len;

  if (total == 0)
    {
      g_array_free (samples, TRUE);
      if (cancelled (cancel, cancel_data))
        return FALSE; /* error already set by the cancel path */
      set_job_error (error, OE_MEDIA_JOB_ERROR_UNSUPPORTED, path, "no audio samples decoded");
      return FALSE;
    }

  gfloat *peaks = g_new (gfloat, (gsize) OE_WAVEFORM_BUCKETS * 2);
  const gsize bucket_size = (total + OE_WAVEFORM_BUCKETS - 1) / OE_WAVEFORM_BUCKETS;

  for (gint b = 0; b < OE_WAVEFORM_BUCKETS; b++)
    {
      gsize start = (gsize) b * bucket_size;
      gsize end = MIN (start + bucket_size, total);
      gint16 lo = G_MAXINT16;
      gint16 hi = G_MININT16;

      if (start >= end)
        {
          lo = hi = 0; /* tail bucket beyond the last sample: silence */
        }
      else
        {
          for (gsize i = start; i < end; i++)
            {
              gint16 s = g_array_index (samples, gint16, (guint) i);

              lo = MIN (lo, s);
              hi = MAX (hi, s);
            }
        }

      peaks[(gsize) b * 2] = (gfloat) lo / 32768.0f;
      peaks[(gsize) b * 2 + 1] = (gfloat) hi / 32768.0f;
    }

  g_array_free (samples, TRUE);

  out->bucket_count = OE_WAVEFORM_BUCKETS;
  out->peaks = peaks;

  oe_log (OE_LOG_LEVEL_DEBUG, "waveform for '%s': %d buckets over %u samples", path,
          OE_WAVEFORM_BUCKETS, (guint) total);
  return TRUE;
}
