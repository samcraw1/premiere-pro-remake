/* oe_export.c — GTK-free synchronous MP4 export job (Phase 8).
 *
 * Pipeline per run: mixdown audio (decode → swr → float sum → clamp),
 * pump the frame grid through the render seam (sequential decode, no
 * per-frame seeks), encode H.264 (libx264 by name, by-id fallback) and
 * AAC (native), and mux to MP4 through a custom AVIOContext writing
 * straight to a g_mkstemp temp file in the destination directory.
 * Finalization is write_atomic's shape: trailer → flush → fsync the
 * temp fd → g_rename over the destination; every other path unlinks
 * the temp and leaves the destination byte-identical.
 *
 * The decode idioms (packet pump, stream picking) mirror
 * oe_media_playback.c, which mirrors oe_media_jobs.c. FFmpeg
 * identifiers never leave this file (adapter leak rule, oe_ffmpeg.h).
 */

#include "oe_export.h"

#include "../core/oe_audio_factor.h"
#include "../core/oe_fades.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "oe_render.h"

#define OE_EXPORT_SAMPLE_RATE 48000
#define OE_EXPORT_CHANNELS 2

/* Quality preset → x264 CRF (locked decision: 18/23/28, veryfast). */
static const gchar *
crf_for_quality (OeExportQuality quality)
{
  switch (quality)
    {
    case OE_EXPORT_QUALITY_HIGH:
      return "18";
    case OE_EXPORT_QUALITY_LOW:
      return "28";
    case OE_EXPORT_QUALITY_MEDIUM:
    default:
      return "23";
    }
}

GQuark
oe_export_error_quark (void)
{
  return g_quark_from_static_string ("oe-export-error");
}

/* ------------------------------------------------------------------ */
/* Frame grid (integer arithmetic only)                                */
/* ------------------------------------------------------------------ */

/* Max clip end over all tracks — the end-of-sequence rule, mirrored
 * from oe_playback_session.c (the model's only defined end). */
static gint64
export_sequence_end (const OeSequence *sequence)
{
  gint64 end = 0;

  if (sequence->tracks == NULL)
    return 0;

  for (guint t = 0; t < sequence->tracks->len; t++)
    {
      const OeTrack *track = g_ptr_array_index (sequence->tracks, t);

      if (track->clips == NULL)
        continue;

      for (guint c = 0; c < track->clips->len; c++)
        {
          const OeClip *clip = g_ptr_array_index (track->clips, c);
          end = MAX (end, clip->position_us + (clip->source_out_us - clip->source_in_us));
        }
    }

  return end;
}

gint64
oe_export_frame_count (const OeSequence *sequence)
{
  g_return_val_if_fail (sequence != NULL, 0);

  const gint64 end_us = export_sequence_end (sequence);

  if (end_us <= 0)
    return 0;

  const gint64 interval_us = oe_time_frame_to_us (1, sequence->frame_rate);

  if (interval_us <= 0)
    return 0;

  return (end_us + interval_us - 1) / interval_us;
}

gint64
oe_export_frame_time_us (gint64 frame_index, const OeSequence *sequence)
{
  g_return_val_if_fail (sequence != NULL, 0);

  return oe_time_frame_to_us (frame_index, sequence->frame_rate);
}

/* ------------------------------------------------------------------ */
/* Export-local decode plumbing (idiom from oe_media_playback.c)       */
/* ------------------------------------------------------------------ */

typedef struct
{
  AVFormatContext *fmt;
  AVCodecContext *ctx;
  AVStream *stream;
  int stream_index;
} ExportDecoder;

static void
export_decoder_close (ExportDecoder *d)
{
  if (d->ctx != NULL)
    avcodec_free_context (&d->ctx);

  if (d->fmt != NULL)
    avformat_close_input (&d->fmt);

  d->stream = NULL;
  d->stream_index = -1;
}

/* Shared opener: first stream of @wanted_type, skipping attached
 * cover art; mirrors decoder_open in oe_media_playback.c. */
static gboolean
export_decoder_open (const gchar *path, enum AVMediaType wanted_type, ExportDecoder *d,
                     GError **error)
{
  memset (d, 0, sizeof (*d));
  d->stream_index = -1;

  if (avformat_open_input (&d->fmt, path, NULL, NULL) != 0)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "'%s': cannot open file", path);
      return FALSE;
    }

  if (avformat_find_stream_info (d->fmt, NULL) < 0)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "'%s': cannot probe streams",
                   path);
      export_decoder_close (d);
      return FALSE;
    }

  for (unsigned i = 0; i < d->fmt->nb_streams; i++)
    {
      AVStream *st = d->fmt->streams[i];

      if (st->codecpar == NULL || st->codecpar->codec_type != wanted_type)
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
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "'%s': no usable %s stream",
                   path, wanted_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio");
      export_decoder_close (d);
      return FALSE;
    }

  return TRUE;
}

/* Sends one packet and consumes it either way. */
static void
export_send_packet (AVCodecContext *ctx, AVPacket *pkt)
{
  avcodec_send_packet (ctx, pkt);
  av_packet_unref (pkt);
}

/* First presentation timestamp in stream time_base units, or 0. */
static gint64
export_frame_pts (const AVFrame *frame)
{
  if (frame->pts != AV_NOPTS_VALUE)
    return frame->pts;

  if (frame->pkt_dts != AV_NOPTS_VALUE)
    return frame->pkt_dts;

  return 0;
}

/* Decodes the next frame — no time filter; callers apply policy.
 * Sets *end_of_stream (without touching @error) at stream end. */
static gboolean
export_read_next_frame (ExportDecoder *d, AVFrame *out, gboolean *end_of_stream, GError **error)
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
          g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "frame decode failed");
          return FALSE;
        }

      if (!draining)
        {
          AVPacket *pkt = av_packet_alloc ();

          g_assert_nonnull (pkt);

          if (av_read_frame (d->fmt, pkt) == 0)
            {
              if (pkt->stream_index == d->stream_index)
                export_send_packet (d->ctx, pkt);
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
/* Audio mixdown                                                       */
/* ------------------------------------------------------------------ */

typedef struct
{
  ExportDecoder dec;
  SwrContext *swr;
  gint64 next_src_us; /* start of the next expected source frame */
} MixSource;

static void
mix_source_free (gpointer data)
{
  MixSource *ms = data;

  export_decoder_close (&ms->dec);
  if (ms->swr != NULL)
    swr_free (&ms->swr);
  g_free (ms);
}

/* Samples a source frame spans, in µs (bounded against div by zero). */
static gint64
frame_span_us (const AVFrame *frame)
{
  if (frame->sample_rate <= 0)
    return 0;

  return (gint64) ((double) frame->nb_samples * 1000000.0 / (double) frame->sample_rate);
}

/* Decodes all audio tracks additively into an interleaved-stereo
 * float mix of @total_samples samples. Gaps stay silent (zeroed);
 * every audible track contributes in array order through the shared
 * integer factor chain (Phase 10 Wave A): fade × clip gain/pan ×
 * track volume/pan, with the mute/solo matrix zeroing silenced
 * tracks — a silenced track is skipped before its media is even
 * opened. Sums are hard-clamped once, at the end. */
static gboolean
export_mixdown (const OeExportSpec *spec, gint64 total_samples, gfloat *mix,
                OeExportCancelFunc cancel_fn, gpointer cancel_data, GError **error)
{
  GHashTable *sources
      = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, mix_source_free);
  gboolean ok = FALSE;

  /* The any-solo scan (D5): one pass over the audio tracks — video
   * tracks carry no audio state and never count. */
  gboolean any_solo = FALSE;

  for (guint t = 0; t < spec->sequence->tracks->len && !any_solo; t++)
    {
      const OeTrack *track = g_ptr_array_index (spec->sequence->tracks, t);

      any_solo = track->kind == OE_TRACK_AUDIO && track->audio.solo != 0;
    }

  for (guint t = 0; t < spec->sequence->tracks->len; t++)
    {
      const OeTrack *track = g_ptr_array_index (spec->sequence->tracks, t);

      if (track->kind != OE_TRACK_AUDIO || track->clips == NULL)
        continue;

      /* The track-level mute/solo verdict (D5) for this track. */
      const gboolean audible = oe_audio_audible (track->audio.mute, track->audio.solo, any_solo);

      for (guint c = 0; c < track->clips->len; c++)
        {
          if (cancel_fn != NULL && cancel_fn (cancel_data))
            {
              g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_CANCELLED,
                           "export cancelled during audio mixdown");
              goto out;
            }

          const OeClip *clip = g_ptr_array_index (track->clips, c);
          const gint64 clip_start_us = clip->position_us;
          const gint64 clip_len_us = clip->source_out_us - clip->source_in_us;
          const gint64 clip_end_us = clip_start_us + clip_len_us;

          if (clip_start_us >= export_sequence_end (spec->sequence))
            continue; /* entirely past the sequence end */

          if (!audible)
            continue; /* muted or lost-solo: the track contributes silence */

          MixSource *ms = g_hash_table_lookup (sources, GUINT_TO_POINTER (clip->media_ref));

          if (ms == NULL)
            {
              gchar *path = spec->resolve_path (clip->media_ref, spec->resolve_data);

              if (path == NULL)
                {
                  g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED,
                               "audio mixdown: media reference %u has no file path",
                               clip->media_ref);
                  goto out;
                }

              ms = g_new0 (MixSource, 1);

              if (!export_decoder_open (path, AVMEDIA_TYPE_AUDIO, &ms->dec, error))
                {
                  g_free (path);
                  g_free (ms);
                  goto out;
                }

              g_free (path);
              g_hash_table_insert (sources, GUINT_TO_POINTER (clip->media_ref), ms);
            }

          /* Position the source's decode cursor for this clip's
           * window: forward clips keep reading; a backward clip seeks
           * (same discipline as the video path). */
          const gint64 clip_src_start_us = clip->source_in_us;

          if (clip_src_start_us < ms->next_src_us)
            {
              const gint64 want_tb
                  = av_rescale_q (clip_src_start_us, AV_TIME_BASE_Q, ms->dec.stream->time_base);

              if (av_seek_frame (ms->dec.fmt, ms->dec.stream_index, want_tb, AVSEEK_FLAG_BACKWARD)
                  >= 0)
                avcodec_flush_buffers (ms->dec.ctx);
              ms->next_src_us = clip_src_start_us;
            }

          AVFrame *frame = av_frame_alloc ();

          g_assert_nonnull (frame);

          gboolean clip_done = FALSE;

          while (!clip_done)
            {
              gboolean end_of_stream = FALSE;

              if (!export_read_next_frame (&ms->dec, frame, &end_of_stream, error))
                {
                  if (end_of_stream)
                    break; /* source shorter than the clip window */

                  goto frame_error;
                }

              const gint64 pts_us = av_rescale_q (export_frame_pts (frame),
                                                  ms->dec.stream->time_base, AV_TIME_BASE_Q);
              const gint64 frame_src_us = MAX (pts_us, ms->next_src_us);
              const gint64 frame_end_src_us = frame_src_us + frame_span_us (frame);

              if (frame_end_src_us <= clip_src_start_us)
                continue; /* entirely before the clip window */

              /* Lazily build the resampler from the first decoded
               * frame's format — the decoder reports layout/rate here. */
              if (ms->swr == NULL)
                {
                  AVChannelLayout out_layout;

                  av_channel_layout_default (&out_layout, OE_EXPORT_CHANNELS);

                  if (swr_alloc_set_opts2 (&ms->swr, &out_layout, AV_SAMPLE_FMT_FLT,
                                           OE_EXPORT_SAMPLE_RATE, &frame->ch_layout,
                                           (enum AVSampleFormat) frame->format, frame->sample_rate,
                                           0, NULL)
                          < 0
                      || swr_init (ms->swr) < 0)
                    {
                      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED,
                                   "audio resampler setup failed");
                      goto frame_error;
                    }
                }

              /* Convert into scratch, then add the in-window span into
               * the mix at the frame's sequence-time position. */
              const int want_out = swr_get_out_samples (ms->swr, frame->nb_samples);
              const int alloc_out = MAX (want_out, frame->nb_samples);
              gfloat *scratch = g_malloc ((gsize) alloc_out * OE_EXPORT_CHANNELS * sizeof (float));
              uint8_t *out_arr[1] = { (uint8_t *) scratch };
              const int converted
                  = swr_convert (ms->swr, out_arr, alloc_out,
                                 (const uint8_t **) frame->extended_data, frame->nb_samples);

              if (converted < 0)
                {
                  g_free (scratch);
                  g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED,
                               "audio resample failed");
                  goto frame_error;
                }

              ms->next_src_us = frame_src_us + frame_span_us (frame);

              /* Head trim: samples before the clip's source window
               * start drop; the rest land at (src - in + clip_start). */
              gint64 write_idx = av_rescale (frame_src_us - clip_src_start_us + clip_start_us,
                                             OE_EXPORT_SAMPLE_RATE, 1000000);
              gint64 skip = 0;

              if (write_idx < 0)
                {
                  skip = -write_idx;
                  write_idx = 0;
                }

              gint64 n_write = (gint64) converted - skip;

              if (write_idx + n_write > total_samples)
                n_write = total_samples - write_idx;

              /* Shared gain envelope (Wave B): one implementation
               * for preview and export. Sequence time of output
               * sample (skip + i) is clip-relative resample math in
               * integer µs. */
              const guint gain = oe_fade_gain (
                  clip_start_us + (frame_src_us - clip_src_start_us)
                      + av_rescale (skip, G_USEC_PER_SEC, OE_EXPORT_SAMPLE_RATE),
                  clip_start_us, clip_end_us, clip->visual.fade_in_us, clip->visual.fade_out_us);

              /* Shared factor chain (Phase 10 Wave A): the fade keeps
               * its per-AVFrame cadence while clip gain/pan and track
               * volume/pan are buffer-constant; the chain folds both
               * pan pairs, zeroes the buffer when the matrix
               * silenced the track, and stays integer end to end. */
              gint32 factor[2] = { 0, 0 };

              oe_audio_factor ((gint32) gain, clip->audio.gain, clip->audio.pan,
                               track->audio.volume, track->audio.pan, audible ? 1 : 0, factor);

              if (factor[0] != 0 || factor[1] != 0)
                {
                  for (gint64 i = 0; i < n_write; i++)
                    {
                      const gfloat *s = scratch + (size_t) (skip + i) * OE_EXPORT_CHANNELS;
                      gfloat *d = mix + (size_t) (write_idx + i) * OE_EXPORT_CHANNELS;

                      d[0] += s[0] * (gfloat) factor[0] / (gfloat) OE_AUDIO_UNITY;
                      d[1] += s[1] * (gfloat) factor[1] / (gfloat) OE_AUDIO_UNITY;
                    }
                }

              g_free (scratch);

              if (frame_end_src_us >= clip_end_us || write_idx + n_write >= total_samples)
                clip_done = TRUE;
            }

          av_frame_free (&frame);
          continue;

        frame_error:
          av_frame_free (&frame);
          goto out;
        }
    }

  /* Hard clamp to ±1.0 across the whole mix. */
  for (gint64 i = 0; i < total_samples * OE_EXPORT_CHANNELS; i++)
    mix[i] = CLAMP (mix[i], -1.0f, 1.0f);

  ok = TRUE;

out:
  g_clear_pointer (&sources, g_hash_table_unref);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Encoders                                                            */
/* ------------------------------------------------------------------ */

typedef struct
{
  AVCodecContext *ctx;
  AVStream *stream;
} ExportEncoder;

static void
export_encoder_free (ExportEncoder *e)
{
  if (e->ctx != NULL)
    avcodec_free_context (&e->ctx);
  e->stream = NULL;
}

/* H.264: libx264 by name, any installed encoder by id. CRF and the
 * veryfast preset ride the private-option dict; a non-x264 fallback
 * simply leaves the unknown options in the dict (freed below). */
static gboolean
export_video_encoder_new (const OeSequence *sequence, OeExportQuality quality, AVFormatContext *fmt,
                          ExportEncoder *e, GError **error)
{
  const AVCodec *codec = avcodec_find_encoder_by_name ("libx264");

  if (codec == NULL)
    codec = avcodec_find_encoder (AV_CODEC_ID_H264);

  if (codec == NULL)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_ENCODER, "no H.264 encoder available");
      return FALSE;
    }

  memset (e, 0, sizeof (*e));

  e->ctx = avcodec_alloc_context3 (codec);

  g_assert_nonnull (e->ctx);

  const OeRational rate = sequence->frame_rate;

  e->ctx->width = sequence->width;
  e->ctx->height = sequence->height;
  e->ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  e->ctx->time_base = (AVRational) { (int) rate.den, (int) rate.num };
  e->ctx->framerate = (AVRational) { (int) rate.num, (int) rate.den };

  if ((fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0)
    e->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  AVDictionary *opts = NULL;

  av_dict_set (&opts, "crf", crf_for_quality (quality), 0);
  av_dict_set (&opts, "preset", "veryfast", 0);

  const int rc = avcodec_open2 (e->ctx, codec, &opts);

  av_dict_free (&opts);

  if (rc < 0)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_ENCODER, "H.264 encoder open failed");
      export_encoder_free (e);
      return FALSE;
    }

  e->stream = avformat_new_stream (fmt, NULL);

  if (e->stream == NULL)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_ENCODER, "cannot add video stream");
      export_encoder_free (e);
      return FALSE;
    }

  e->stream->time_base = e->ctx->time_base;

  if (avcodec_parameters_from_context (e->stream->codecpar, e->ctx) < 0)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_ENCODER,
                   "video codec parameters failed");
      export_encoder_free (e);
      return FALSE;
    }

  return TRUE;
}

/* Native AAC at 48 kHz stereo, planar float. */
static gboolean
export_audio_encoder_new (AVFormatContext *fmt, ExportEncoder *e, GError **error)
{
  const AVCodec *codec = avcodec_find_encoder (AV_CODEC_ID_AAC);

  if (codec == NULL)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_ENCODER, "no AAC encoder available");
      return FALSE;
    }

  memset (e, 0, sizeof (*e));

  e->ctx = avcodec_alloc_context3 (codec);

  g_assert_nonnull (e->ctx);

  e->ctx->sample_rate = OE_EXPORT_SAMPLE_RATE;
  av_channel_layout_default (&e->ctx->ch_layout, OE_EXPORT_CHANNELS);
  e->ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
  e->ctx->time_base = (AVRational) { 1, OE_EXPORT_SAMPLE_RATE };

  if ((fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0)
    e->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  if (avcodec_open2 (e->ctx, codec, NULL) < 0)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_ENCODER, "AAC encoder open failed");
      export_encoder_free (e);
      return FALSE;
    }

  e->stream = avformat_new_stream (fmt, NULL);

  if (e->stream == NULL)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_ENCODER, "cannot add audio stream");
      export_encoder_free (e);
      return FALSE;
    }

  e->stream->time_base = e->ctx->time_base;

  if (avcodec_parameters_from_context (e->stream->codecpar, e->ctx) < 0)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_ENCODER,
                   "audio codec parameters failed");
      export_encoder_free (e);
      return FALSE;
    }

  return TRUE;
}

/* Sends one frame (NULL flushes) and drains every ready packet. */
static gboolean
export_encode_frame (AVCodecContext *ctx, AVStream *stream, AVFormatContext *fmt, AVFrame *frame,
                     GError **error)
{
  const int rc_send = avcodec_send_frame (ctx, frame);

  if (rc_send < 0 && rc_send != AVERROR_EOF)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "encoder rejected a frame");
      return FALSE;
    }

  for (;;)
    {
      AVPacket *pkt = av_packet_alloc ();

      g_assert_nonnull (pkt);

      const int rc = avcodec_receive_packet (ctx, pkt);

      if (rc == AVERROR (EAGAIN) || rc == AVERROR_EOF)
        {
          av_packet_free (&pkt);
          return TRUE;
        }

      if (rc < 0)
        {
          av_packet_free (&pkt);
          g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "encode failed");
          return FALSE;
        }

      av_packet_rescale_ts (pkt, ctx->time_base, stream->time_base);
      pkt->stream_index = stream->index;

      const int rc_mux = av_interleaved_write_frame (fmt, pkt);

      av_packet_free (&pkt);

      if (rc_mux < 0)
        {
          g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "mux write failed");
          return FALSE;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Custom AVIO over the temp fd (write_atomic parity)                  */
/* ------------------------------------------------------------------ */

static int
avio_write_fn (void *opaque, const uint8_t *buf, int buf_size)
{
  const int fd = GPOINTER_TO_INT (opaque);
  int written = 0;

  while (written < buf_size)
    {
      const ssize_t n = write (fd, buf + written, (size_t) (buf_size - written));

      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          return AVERROR (errno);
        }

      written += (int) n;
    }

  return written;
}

static int64_t
avio_seek_fn (void *opaque, int64_t offset, int whence)
{
  const int fd = GPOINTER_TO_INT (opaque);

  if ((whence & AVSEEK_SIZE) != 0)
    {
      const int64_t save = lseek (fd, 0, SEEK_CUR);
      const int64_t size = lseek (fd, 0, SEEK_END);

      lseek (fd, save, SEEK_SET);
      return size < 0 ? AVERROR (errno) : size;
    }

  const int64_t rc = lseek (fd, offset, whence & ~AVSEEK_FORCE);

  return rc < 0 ? AVERROR (errno) : rc;
}

/* ------------------------------------------------------------------ */
/* The job                                                             */
/* ------------------------------------------------------------------ */

/* Every per-run resource, so the epilogue frees exactly once. */
typedef struct
{
  AVFormatContext *fmt;
  AVIOContext *avio;
  int fd;
  gchar *tmp_path;
  ExportEncoder video;
  ExportEncoder audio;
  struct SwsContext *sws;
} ExportJob;

/* Failure-path teardown: discard buffered output, unlink the temp. */
static void
export_job_teardown (ExportJob *job)
{
  /* The avio buffer stays caller-owned (see alloc site): capture the
   * possibly-grown buffer, free the context, then free the buffer. */
  if (job->avio != NULL)
    {
      uint8_t *avio_buf = job->avio->buffer;

      avio_context_free (&job->avio);
      av_free (avio_buf);
    }

  if (job->sws != NULL)
    sws_freeContext (job->sws);

  export_encoder_free (&job->video);
  export_encoder_free (&job->audio);

  if (job->fmt != NULL)
    avformat_free_context (job->fmt);

  if (job->fd >= 0)
    {
      g_close (job->fd, NULL);
      job->fd = -1;
    }

  if (job->tmp_path != NULL)
    {
      g_unlink (job->tmp_path); /* every non-success path */
      g_clear_pointer (&job->tmp_path, g_free);
    }
}

gboolean
oe_export_run (const OeExportSpec *spec, OeExportCancelFunc cancel_fn, gpointer cancel_data,
               OeExportProgressFunc progress_fn, gpointer progress_data, GError **error)
{
  g_return_val_if_fail (spec != NULL, FALSE);
  g_return_val_if_fail (spec->sequence != NULL, FALSE);
  g_return_val_if_fail (spec->resolve_path != NULL, FALSE);
  g_return_val_if_fail (spec->destination_path != NULL && spec->destination_path[0] != '\0', FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  const OeSequence *sequence = spec->sequence;

  g_return_val_if_fail (sequence->width > 0 && sequence->height > 0, FALSE);

  const gint64 total = oe_export_frame_count (sequence);

  if (total <= 0)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED,
                   "nothing to export: the sequence has no clips");
      return FALSE;
    }

  if (cancel_fn != NULL && cancel_fn (cancel_data))
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_CANCELLED,
                   "export cancelled before it started");
      return FALSE;
    }

  ExportJob job = { 0 };

  job.fd = -1;

  const gchar *dest = spec->destination_path;
  gchar *dir = g_path_get_dirname (dest);
  OeRenderSource render_source = { sequence, spec->resolve_path, spec->resolve_data };
  gfloat *mix = NULL;
  gint64 total_samples = 0;
  gboolean ok = FALSE;

  /* Temp file in the TARGET directory, write_atomic style. */
  job.tmp_path = g_build_filename (dir, ".oe-export-XXXXXX", NULL);

  job.fd = g_mkstemp (job.tmp_path);

  if (job.fd < 0)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_OPEN_FAILED,
                   "cannot create temp file in '%s': %s", dir, g_strerror (errno));
      goto out;
    }

  fchmod (job.fd, 0644);

  /* MP4 output context over the custom AVIO. */
  if (avformat_alloc_output_context2 (&job.fmt, NULL, "mp4", dest) < 0 || job.fmt == NULL)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "MP4 context setup failed");
      goto out;
    }

  guint8 *avio_buf = av_malloc (64 * 1024);

  g_assert_nonnull (avio_buf);

  /* The buffer stays caller-owned for the whole run: every teardown
   * path frees the context's current buffer after avio_context_free. */
  job.avio = avio_alloc_context (avio_buf, 64 * 1024, 1, GINT_TO_POINTER (job.fd), NULL,
                                 avio_write_fn, avio_seek_fn);

  g_assert_nonnull (job.avio);

  job.fmt->pb = job.avio;

  /* Encoders + streams. */
  if (!export_video_encoder_new (sequence, spec->quality, job.fmt, &job.video, error))
    goto out;

  total_samples = av_rescale (export_sequence_end (sequence), OE_EXPORT_SAMPLE_RATE, 1000000);

  if (!export_audio_encoder_new (job.fmt, &job.audio, error))
    goto out;

  if (avformat_write_header (job.fmt, NULL) < 0)
    {
      g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "MP4 header write failed");
      goto out;
    }

  /* ---- Audio mixdown + AAC pump ---- */
  mix = g_new0 (gfloat, (gsize) total_samples * OE_EXPORT_CHANNELS);

  if (!export_mixdown (spec, total_samples, mix, cancel_fn, cancel_data, error))
    goto out;

  {
    const int chunk = job.audio.ctx->frame_size > 0 ? job.audio.ctx->frame_size : 1024;
    AVFrame *pcm = av_frame_alloc ();

    g_assert_nonnull (pcm);

    pcm->format = AV_SAMPLE_FMT_FLTP;
    pcm->sample_rate = OE_EXPORT_SAMPLE_RATE;
    av_channel_layout_copy (&pcm->ch_layout, &job.audio.ctx->ch_layout);
    pcm->nb_samples = chunk;

    if (av_frame_get_buffer (pcm, 0) < 0)
      {
        av_frame_free (&pcm);
        g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED,
                     "audio frame allocation failed");
        goto out;
      }

    for (gint64 pos = 0; pos < total_samples; pos += chunk)
      {
        if (cancel_fn != NULL && cancel_fn (cancel_data))
          {
            av_frame_free (&pcm);
            g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_CANCELLED,
                         "export cancelled during audio encode");
            goto out;
          }

        memset (pcm->data[0], 0, (size_t) chunk * sizeof (float));
        memset (pcm->data[1], 0, (size_t) chunk * sizeof (float));

        const gint64 n = MIN (chunk, total_samples - pos);

        for (gint64 i = 0; i < n; i++)
          {
            ((float *) pcm->data[0])[i] = mix[(size_t) (pos + i) * 2];
            ((float *) pcm->data[1])[i] = mix[(size_t) (pos + i) * 2 + 1];
          }

        pcm->pts = pos;

        if (!export_encode_frame (job.audio.ctx, job.audio.stream, job.fmt, pcm, error))
          {
            av_frame_free (&pcm);
            goto out;
          }
      }

    av_frame_free (&pcm);

    if (!export_encode_frame (job.audio.ctx, job.audio.stream, job.fmt, NULL, error))
      goto out;
  }

  /* ---- Video pump: render grid → yuv420p → H.264 ---- */
  {
    AVFrame *yuv = av_frame_alloc ();

    g_assert_nonnull (yuv);

    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = sequence->width;
    yuv->height = sequence->height;

    if (av_frame_get_buffer (yuv, 0) < 0)
      {
        av_frame_free (&yuv);
        g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED,
                     "video frame allocation failed");
        goto out;
      }

    OeRenderSession *render = oe_render_session_new (&render_source);

    if (render == NULL)
      {
        av_frame_free (&yuv);
        g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "render session setup failed");
        goto out;
      }

    for (gint64 f = 0; f < total; f++)
      {
        if (cancel_fn != NULL && cancel_fn (cancel_data))
          {
            g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_CANCELLED,
                         "export cancelled during video encode");
            goto video_fail;
          }

        GError *render_error = NULL;
        const gint64 t_us = oe_export_frame_time_us (f, sequence);
        guint8 *canvas = oe_render_session_frame_at (render, t_us, sequence->width,
                                                     sequence->height, &render_error);

        if (canvas == NULL)
          {
            g_propagate_error (error, render_error);
            goto video_fail;
          }

        const guint8 *src_data[4] = { canvas, NULL, NULL, NULL };
        const int src_stride[4] = { sequence->width * 4, 0, 0, 0 };

        job.sws = sws_getCachedContext (job.sws, sequence->width, sequence->height, AV_PIX_FMT_BGRA,
                                        sequence->width, sequence->height, AV_PIX_FMT_YUV420P,
                                        SWS_BILINEAR, NULL, NULL, NULL);

        if (job.sws == NULL)
          {
            g_free (canvas);
            g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "swscale setup failed");
            goto video_fail;
          }

        uint8_t *dst_data[4] = { yuv->data[0], yuv->data[1], yuv->data[2], NULL };
        int dst_stride[4] = { yuv->linesize[0], yuv->linesize[1], yuv->linesize[2], 0 };

        sws_scale (job.sws, src_data, src_stride, 0, sequence->height, dst_data, dst_stride);
        g_free (canvas);

        yuv->pts = f;

        if (!export_encode_frame (job.video.ctx, job.video.stream, job.fmt, yuv, error))
          goto video_fail;

        if (progress_fn != NULL)
          progress_fn (f + 1, total, progress_data);
      }

    if (!export_encode_frame (job.video.ctx, job.video.stream, job.fmt, NULL, error))
      goto video_fail;

    oe_render_session_free (render);
    av_frame_free (&yuv);

    /* ---- Finalize: trailer → fsync → atomic rename ---- */
    if (av_write_trailer (job.fmt) < 0)
      {
        g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "MP4 trailer write failed");
        goto out;
      }

    avio_flush (job.avio);

    if (fsync (job.fd) != 0)
      {
        g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED, "temp file flush failed: %s",
                     g_strerror (errno));
        goto out;
      }

    /* Release the fd and AVIO before the rename. */
    uint8_t *avio_buf = job.avio->buffer;

    avio_context_free (&job.avio);
    av_free (avio_buf);
    job.fmt->pb = NULL;
    g_close (job.fd, NULL);
    job.fd = -1;

    if (g_rename (job.tmp_path, dest) != 0)
      {
        g_set_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED,
                     "cannot move export into place: %s", g_strerror (errno));
        goto out;
      }

    g_clear_pointer (&job.tmp_path, g_free); /* renamed — not to be unlinked */
    ok = TRUE;
    goto out;

  video_fail:
    oe_render_session_free (render);
    av_frame_free (&yuv);
    goto out;
  }

out:
  g_free (dir);
  g_free (mix);
  export_job_teardown (&job);
  return ok;
}
