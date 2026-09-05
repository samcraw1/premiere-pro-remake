/* test_export.c — GTK-free tests for the Phase 8 export job (16th suite).
 *
 * Synthetic in-process fixtures only (the fixture_media pattern, extended
 * with two tiny generators of our own): solid-color MJPEG AVIs and a
 * constant-amplitude pcm_s16le WAV. Every assertion is structural —
 * dominant-color classes, probe metadata, mean-amplitude windows,
 * directory emptiness — never bit-exact hashes, so lossy encoders stay
 * inside their contracts. Sizes stay ≤ 160x120 and ~25 frames so the
 * ASan/UBSan/Valgrind matrix remains fast.
 *
 *   /export/frame-grid         integer ceil grid + per-frame times
 *   /export/parity-straight-cut  seam dominance either side of a cut
 *   /export/container-truth    probe: h264 + aac, size, ±1-frame duration
 *   /export/content-round-trip decode exported frame/audio back in-tree
 *   /export/mixdown-sums       two overlapping tracks sum; gaps silent
 *   /export/transition-blend   Wave B ramp: degenerate edges + midpoint
 *   /export/mixdown-fade-ratio shared envelope shapes the mixdown
 *   /export/parity-transition-fade  2 layers + transition + fade parity
 *   /export/mixdown-pan-positions  clip/track pan pairs steer channels
 *   /export/mixdown-gain-ratio     clip gain and track volume halve the mix
 *   /export/mixdown-mute-solo-matrix  serialized D5 matrix on the export path
 *   /export/cancellation       cancel mid-run: typed error, no files left
 *   /export/atomic-failure     failed export leaves the destination
 *                              byte-identical and no temp behind
 *
 * The "cancel mid-trailer" arm of the atomic-failure contract is not
 * externally inducible — the job's cancel checks sit in the frame loop,
 * and the trailer→fsync→rename tail is intentionally finish-or-fail —
 * so the atomic contract is exercised through a mid-stream decode
 * failure and an unwritable destination instead: same invariants
 * (destination byte-identical, temp removed, typed error).
 */

#include <glib.h>
#include <glib/gstdio.h>

#include <math.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "../src/app/oe_playback_session.h"
#include "../src/core/oe_project.h"
#include "../src/core/oe_time.h"
#include "../src/media/oe_export.h"
#include "../src/media/oe_probe.h"
#include "../src/media/oe_render.h"
#include "../src/playback/oe_audio_output.h"
#include "fixture_media.h"

/* Test sequence geometry (even, ≤ 320x240, fast under sanitizers). */
#define TEST_W 160
#define TEST_H 120
#define TEST_FPS 25
#define TEST_FRAME_US (1000000 / TEST_FPS)

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static void
fixture_set_up (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  g_assert_true (oe_fixtures_create (fx, &error));
  g_assert_no_error (error);
}

static void
fixture_tear_down (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  oe_fixtures_free (fx);
}

/* Encodes a solid-color MJPEG AVI: every frame identical, so the
 * render seam's dominance assertions measure compositing, not source
 * content. */
static void
write_solid_avi (const OeFixtures *fx, const gchar *name, guint8 r, guint8 g, guint8 b, int frames)
{
  gchar *path = g_build_filename (fx->dir, name, NULL);
  AVFormatContext *fmt = NULL;

  g_assert_cmpint (avformat_alloc_output_context2 (&fmt, NULL, "avi", path), >=, 0);
  g_assert_nonnull (fmt);

  const AVCodec *enc = avcodec_find_encoder (AV_CODEC_ID_MJPEG);

  g_assert_nonnull (enc);

  AVCodecContext *ctx = avcodec_alloc_context3 (enc);

  g_assert_nonnull (ctx);
  ctx->width = TEST_W;
  ctx->height = TEST_H;
  ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
  ctx->time_base = (AVRational) { 1, TEST_FPS };

  if ((fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0)
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  g_assert_cmpint (avcodec_open2 (ctx, enc, NULL), >=, 0);

  AVStream *st = avformat_new_stream (fmt, NULL);

  g_assert_nonnull (st);
  st->time_base = ctx->time_base;
  g_assert_cmpint (avcodec_parameters_from_context (st->codecpar, ctx), >=, 0);
  g_assert_false ((fmt->oformat->flags & AVFMT_NOFILE) != 0);
  g_assert_cmpint (avio_open (&fmt->pb, path, AVIO_FLAG_WRITE), >=, 0);
  g_assert_cmpint (avformat_write_header (fmt, NULL), >=, 0);

  /* One solid BGRA canvas scaled once into the reusable YUV frame. */
  guint8 *bgra = g_malloc ((gsize) TEST_W * TEST_H * 4);

  for (int i = 0; i < TEST_W * TEST_H; i++)
    {
      bgra[i * 4 + 0] = b;
      bgra[i * 4 + 1] = g;
      bgra[i * 4 + 2] = r;
      bgra[i * 4 + 3] = 0xff;
    }

  struct SwsContext *to_yuv = sws_getContext (TEST_W, TEST_H, AV_PIX_FMT_BGRA, TEST_W, TEST_H,
                                              AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, NULL, NULL, NULL);

  g_assert_nonnull (to_yuv);

  AVFrame *frame = av_frame_alloc ();

  g_assert_nonnull (frame);
  frame->format = AV_PIX_FMT_YUVJ420P;
  frame->width = TEST_W;
  frame->height = TEST_H;
  g_assert_cmpint (av_frame_get_buffer (frame, 0), >=, 0);

  const uint8_t *src_data[4] = { bgra, NULL, NULL, NULL };
  const int src_stride[4] = { TEST_W * 4, 0, 0, 0 };

  sws_scale (to_yuv, src_data, src_stride, 0, TEST_H, frame->data, frame->linesize);

  for (int i = 0; i < frames; i++)
    {
      frame->pts = i;

      g_assert_cmpint (avcodec_send_frame (ctx, frame), >=, 0);

      for (;;)
        {
          AVPacket *pkt = av_packet_alloc ();

          g_assert_nonnull (pkt);

          const int rc = avcodec_receive_packet (ctx, pkt);

          if (rc == AVERROR (EAGAIN) || rc == AVERROR_EOF)
            {
              av_packet_free (&pkt);
              break;
            }

          g_assert_cmpint (rc, >=, 0);
          av_packet_rescale_ts (pkt, ctx->time_base, st->time_base);
          pkt->stream_index = st->index;
          g_assert_cmpint (av_interleaved_write_frame (fmt, pkt), >=, 0);
          av_packet_free (&pkt);
        }
    }

  g_assert_cmpint (avcodec_send_frame (ctx, NULL), >=, 0);

  for (;;)
    {
      AVPacket *pkt = av_packet_alloc ();

      g_assert_nonnull (pkt);

      const int rc = avcodec_receive_packet (ctx, pkt);

      if (rc == AVERROR (EAGAIN) || rc == AVERROR_EOF)
        {
          av_packet_free (&pkt);
          break;
        }

      g_assert_cmpint (rc, >=, 0);
      av_packet_rescale_ts (pkt, ctx->time_base, st->time_base);
      pkt->stream_index = st->index;
      g_assert_cmpint (av_interleaved_write_frame (fmt, pkt), >=, 0);
      av_packet_free (&pkt);
    }

  g_assert_cmpint (av_write_trailer (fmt), >=, 0);

  av_frame_free (&frame);
  sws_freeContext (to_yuv);
  g_free (bgra);
  avcodec_free_context (&ctx);
  avio_closep (&fmt->pb);
  avformat_free_context (fmt);
  g_free (path);
}

/* A constant-amplitude mono WAV: the decoded mean absolute value equals
 * @amp / 32768 regardless of waveform shape, which is exactly what the
 * mixdown classes assert. */
static void
write_dc_wav (const OeFixtures *fx, const gchar *name, gint16 amp, gint64 useconds)
{
  const gint64 frames = useconds * 22050 / 1000000;
  const guint32 data_bytes = (guint32) (frames * 2);
  const guint32 riff_size = 36 + data_bytes;
  const guint32 rate = 22050;
  const guint32 fmt_size = 16;
  const guint16 audio_format = 1, channels = 1, bits = 16, block_align = 2;
  const guint32 byte_rate = rate * 2;
  gchar *path = g_build_filename (fx->dir, name, NULL);
  FILE *f = fopen (path, "wb");

  g_assert_nonnull (f);

  fwrite ("RIFF", 1, 4, f);
  fwrite (&riff_size, 4, 1, f);
  fwrite ("WAVE", 1, 4, f);
  fwrite ("fmt ", 1, 4, f);
  fwrite (&fmt_size, 4, 1, f);
  fwrite (&audio_format, 2, 1, f);
  fwrite (&channels, 2, 1, f);
  fwrite (&rate, 4, 1, f);
  fwrite (&byte_rate, 4, 1, f);
  fwrite (&block_align, 2, 1, f);
  fwrite (&bits, 2, 1, f);
  fwrite ("data", 1, 4, f);
  fwrite (&data_bytes, 4, 1, f);

  for (gint64 i = 0; i < frames; i++)
    fwrite (&amp, 2, 1, f);

  fclose (f);
  g_free (path);
}

/* ------------------------------------------------------------------ */
/* Model builders                                                      */
/* ------------------------------------------------------------------ */

static OeProject *
new_project_25fps (void)
{
  OeProject *project = oe_project_new ((OeRational) { TEST_FPS, 1 });

  g_assert_cmpint (oe_project_set_sequence_size (project, TEST_W, TEST_H, NULL), !=, 0);
  return project;
}

static guint
add_media (OeProject *project, const gchar *path)
{
  guint ref = oe_project_add_media (project, path);

  g_assert_cmpuint (ref, !=, 0);
  return ref;
}

static void
insert_clip (OeProject *project, guint track, guint ref, gint64 pos_us, gint64 len_us)
{
  GError *error = NULL;

  g_assert_true (oe_project_insert_clip (project, track, ref, pos_us, 0, len_us, &error));
  g_assert_no_error (error);
}

/* Video-only project: solid red then solid blue, straight cut at 0.5 s. */
static OeProject *
build_two_cut_video (const OeFixtures *fx, gchar **red_path, gchar **blue_path)
{
  write_solid_avi (fx, "red.avi", 0xe0, 0x20, 0x20, TEST_FPS / 2);
  write_solid_avi (fx, "blue.avi", 0x20, 0x30, 0xe0, TEST_FPS / 2);

  *red_path = g_build_filename (fx->dir, "red.avi", NULL);
  *blue_path = g_build_filename (fx->dir, "blue.avi", NULL);

  OeProject *project = new_project_25fps ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint red = add_media (project, *red_path);
  const guint blue = add_media (project, *blue_path);

  insert_clip (project, video, red, 0, 500000);
  insert_clip (project, video, blue, 500000, 500000);
  return project;
}

/* The export resolver: media refs come straight from the project's
 * media table, exactly like the window builds its worker-side map. */
static gchar *
test_resolve (guint media_ref, gpointer user_data)
{
  OeProject *project = user_data;
  const guint count = oe_project_get_media_count (project);

  for (guint i = 0; i < count; i++)
    {
      guint ref = 0;
      gchar *path = NULL;

      if (oe_project_get_media (project, i, &ref, &path))
        {
          if (ref == media_ref)
            return path; /* transfer */

          g_free (path);
        }
    }

  return NULL;
}

static gboolean
run_export (OeProject *project, const gchar *dest, OeExportQuality quality,
            OeExportCancelFunc cancel_fn, gpointer cancel_data, OeExportProgressFunc progress_fn,
            gpointer progress_data, GError **error)
{
  /* oe_project_get_sequence hands over a whole fresh copy — the caller
   * storage must be zeroed, not pre-initialized (init would leak its
   * fresh tracks array). */
  OeSequence seq = { 0 };

  oe_project_get_sequence (project, &seq);

  OeExportSpec spec = { 0 };

  spec.sequence = &seq;
  spec.resolve_path = test_resolve;
  spec.resolve_data = project;
  spec.destination_path = dest;
  spec.quality = quality;

  const gboolean ok
      = oe_export_run (&spec, cancel_fn, cancel_data, progress_fn, progress_data, error);

  oe_sequence_clear (&seq);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Decode-back helpers                                                 */
/* ------------------------------------------------------------------ */

/* Decodes the @want_index-th video frame of @path into a BGRA canvas. */
static guint8 *
decode_video_frame_bgra (const gchar *path, gint64 want_index, int *out_w, int *out_h)
{
  AVFormatContext *fmt = NULL;

  g_assert_cmpint (avformat_open_input (&fmt, path, NULL, NULL), >=, 0);
  g_assert_cmpint (avformat_find_stream_info (fmt, NULL), >=, 0);

  int video_stream = -1;

  for (unsigned i = 0; i < fmt->nb_streams; i++)
    {
      AVStream *st = fmt->streams[i];

      if (st->codecpar != NULL && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
          && (st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
        {
          video_stream = (int) i;
          break;
        }
    }

  g_assert_cmpint (video_stream, >=, 0);

  const AVCodec *dec = avcodec_find_decoder (fmt->streams[video_stream]->codecpar->codec_id);

  g_assert_nonnull (dec);

  AVCodecContext *ctx = avcodec_alloc_context3 (dec);

  g_assert_nonnull (ctx);
  g_assert_cmpint (avcodec_parameters_to_context (ctx, fmt->streams[video_stream]->codecpar), >=,
                   0);
  g_assert_cmpint (avcodec_open2 (ctx, dec, NULL), >=, 0);

  *out_w = ctx->width;
  *out_h = ctx->height;

  guint8 *result = NULL;
  AVPacket *pkt = av_packet_alloc ();
  AVFrame *frame = av_frame_alloc ();

  g_assert_nonnull (pkt);
  g_assert_nonnull (frame);

  gint64 seen = 0;
  gboolean draining = FALSE;

  while (result == NULL)
    {
      const int rc = avcodec_receive_frame (ctx, frame);

      if (rc == 0)
        {
          if (seen++ == want_index)
            {
              struct SwsContext *sws = sws_getContext (
                  ctx->width, ctx->height, (enum AVPixelFormat) frame->format, ctx->width,
                  ctx->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
              g_assert_nonnull (sws);

              result = g_malloc ((gsize) ctx->width * ctx->height * 4);

              uint8_t *dst_data[4] = { result, NULL, NULL, NULL };
              int dst_stride[4] = { ctx->width * 4, 0, 0, 0 };

              sws_scale (sws, (const uint8_t *const *) frame->data, frame->linesize, 0, ctx->height,
                         dst_data, dst_stride);
              sws_freeContext (sws);
            }
          continue;
        }

      if (rc == AVERROR_EOF)
        break;

      g_assert_cmpint (rc, ==, AVERROR (EAGAIN));

      if (!draining)
        {
          if (av_read_frame (fmt, pkt) == 0)
            {
              if (pkt->stream_index == video_stream)
                {
                  avcodec_send_packet (ctx, pkt); /* refs internally */
                  av_packet_unref (pkt);          /* ownership stays here */
                }
              else
                av_packet_unref (pkt);
            }
          else
            {
              avcodec_send_packet (ctx, NULL);
              draining = TRUE;
            }
        }
      else
        break;
    }

  av_frame_free (&frame);
  av_packet_free (&pkt);
  avcodec_free_context (&ctx);
  avformat_close_input (&fmt);

  g_assert_nonnull (result); /* the wanted frame must exist */
  return result;
}

/* Decodes the last frame the decoder emits for @path (the final
 * decodable access unit). See the round-trip test for why this is not
 * the same as the last frame index. */
static guint8 *
decode_last_video_frame_bgra (const gchar *path, int *out_w, int *out_h)
{
  AVFormatContext *fmt = NULL;

  g_assert_cmpint (avformat_open_input (&fmt, path, NULL, NULL), >=, 0);
  g_assert_cmpint (avformat_find_stream_info (fmt, NULL), >=, 0);

  int video_stream = -1;

  for (unsigned i = 0; i < fmt->nb_streams; i++)
    {
      AVStream *st = fmt->streams[i];

      if (st->codecpar != NULL && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
          && (st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
        {
          video_stream = (int) i;
          break;
        }
    }

  g_assert_cmpint (video_stream, >=, 0);

  const AVCodec *dec = avcodec_find_decoder (fmt->streams[video_stream]->codecpar->codec_id);

  g_assert_nonnull (dec);

  AVCodecContext *ctx = avcodec_alloc_context3 (dec);

  g_assert_nonnull (ctx);
  g_assert_cmpint (avcodec_parameters_to_context (ctx, fmt->streams[video_stream]->codecpar), >=,
                   0);
  g_assert_cmpint (avcodec_open2 (ctx, dec, NULL), >=, 0);

  *out_w = ctx->width;
  *out_h = ctx->height;

  AVPacket *pkt = av_packet_alloc ();
  AVFrame *frame = av_frame_alloc ();

  g_assert_nonnull (pkt);
  g_assert_nonnull (frame);

  guint8 *result = NULL;
  gboolean draining = FALSE;

  for (;;)
    {
      const int rc = avcodec_receive_frame (ctx, frame);

      if (rc == AVERROR_EOF)
        break;

      if (rc == AVERROR (EAGAIN))
        {
          if (draining)
            break;

          if (av_read_frame (fmt, pkt) == 0)
            {
              if (pkt->stream_index == video_stream)
                {
                  avcodec_send_packet (ctx, pkt); /* refs internally */
                  av_packet_unref (pkt);          /* ownership stays here */
                }
              else
                av_packet_unref (pkt);
            }
          else
            {
              avcodec_send_packet (ctx, NULL);
              draining = TRUE;
            }
          continue;
        }

      g_assert_cmpint (rc, >=, 0);

      struct SwsContext *sws
          = sws_getContext (ctx->width, ctx->height, (enum AVPixelFormat) frame->format, ctx->width,
                            ctx->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
      g_assert_nonnull (sws);

      g_free (result);
      result = g_malloc ((gsize) ctx->width * ctx->height * 4);

      uint8_t *dst_data[4] = { result, NULL, NULL, NULL };
      int dst_stride[4] = { ctx->width * 4, 0, 0, 0 };

      sws_scale (sws, (const uint8_t *const *) frame->data, frame->linesize, 0, ctx->height,
                 dst_data, dst_stride);
      sws_freeContext (sws);
    }

  av_frame_free (&frame);
  av_packet_free (&pkt);
  avcodec_free_context (&ctx);
  avformat_close_input (&fmt);

  g_assert_nonnull (result); /* the stream must decode at least one frame */
  return result;
}

/* Channel-dominance class over the central region (MJPEG rings off the
 * edges must not flip the verdict). */
static void
assert_dominant (const guint8 *bgra, int w, int h, const gchar *what, guint8 r, guint8 g, guint8 b)
{
  glong r_sum = 0, g_sum = 0, b_sum = 0;

  for (int y = h / 4; y < (3 * h) / 4; y++)
    for (int x = w / 4; x < (3 * w) / 4; x++)
      {
        const guint8 *px = bgra + ((gsize) y * w + x) * 4;

        r_sum += px[2];
        g_sum += px[1];
        b_sum += px[0];
      }

  g_assert_cmpstr (what, !=, NULL);

  if (r >= g && r >= b)
    {
      g_assert_cmpint ((int) (r_sum / 2), >, g_sum);
      g_assert_cmpint ((int) (r_sum / 2), >, b_sum);
    }
  else if (g >= r && g >= b)
    {
      g_assert_cmpint ((int) (g_sum / 2), >, r_sum);
      g_assert_cmpint ((int) (g_sum / 2), >, b_sum);
    }
  else
    {
      g_assert_cmpint ((int) (b_sum / 2), >, r_sum);
      g_assert_cmpint ((int) (b_sum / 2), >, g_sum);
    }
}

typedef struct
{
  gfloat *data; /* interleaved stereo, 48 kHz */
  gint64 frames;
} FloatAudio;

/* Decodes every audio sample of @path to interleaved stereo float. */
static void
decode_audio_float (const gchar *path, FloatAudio *out)
{
  AVFormatContext *fmt = NULL;

  g_assert_cmpint (avformat_open_input (&fmt, path, NULL, NULL), >=, 0);
  g_assert_cmpint (avformat_find_stream_info (fmt, NULL), >=, 0);

  int audio_stream = -1;

  for (unsigned i = 0; i < fmt->nb_streams; i++)
    {
      AVStream *st = fmt->streams[i];

      if (st->codecpar != NULL && st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
          && (st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
        {
          audio_stream = (int) i;
          break;
        }
    }

  g_assert_cmpint (audio_stream, >=, 0);

  AVStream *st = fmt->streams[audio_stream];
  const AVCodec *dec = avcodec_find_decoder (st->codecpar->codec_id);

  g_assert_nonnull (dec);

  AVCodecContext *ctx = avcodec_alloc_context3 (dec);

  g_assert_nonnull (ctx);
  g_assert_cmpint (avcodec_parameters_to_context (ctx, st->codecpar), >=, 0);
  g_assert_cmpint (avcodec_open2 (ctx, dec, NULL), >=, 0);

  AVChannelLayout out_layout;

  av_channel_layout_default (&out_layout, 2);

  SwrContext *swr = NULL;

  g_assert_cmpint (swr_alloc_set_opts2 (&swr, &out_layout, AV_SAMPLE_FMT_FLT, 48000,
                                        &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, 0,
                                        NULL),
                   >=, 0);
  g_assert_cmpint (swr_init (swr), >=, 0);

  AVPacket *pkt = av_packet_alloc ();
  AVFrame *frame = av_frame_alloc ();

  g_assert_nonnull (pkt);
  g_assert_nonnull (frame);

  gfloat *buf = NULL;
  gint64 cap = 0, n = 0;
  gboolean draining = FALSE;

  for (;;)
    {
      const int rc = avcodec_receive_frame (ctx, frame);

      if (rc == AVERROR_EOF)
        break;

      if (rc == AVERROR (EAGAIN))
        {
          if (draining)
            break;

          if (av_read_frame (fmt, pkt) == 0)
            {
              if (pkt->stream_index == audio_stream)
                {
                  avcodec_send_packet (ctx, pkt); /* refs internally */
                  av_packet_unref (pkt);          /* ownership stays here */
                }
              else
                av_packet_unref (pkt);
            }
          else
            {
              avcodec_send_packet (ctx, NULL);
              draining = TRUE;
            }
          continue;
        }

      g_assert_cmpint (rc, >=, 0);

      const int want = swr_get_out_samples (swr, frame->nb_samples);

      g_assert_cmpint (want, >, 0);

      if (n + want > cap)
        {
          cap = (n + want) * 2;
          buf = g_realloc (buf, (gsize) cap * sizeof (gfloat) * 2);
        }

      uint8_t *out_arr[1] = { (uint8_t *) (buf + n * 2) };
      const int converted = swr_convert (
          swr, out_arr, want, (const uint8_t **) frame->extended_data, frame->nb_samples);

      g_assert_cmpint (converted, >, 0);
      n += converted;
    }

  swr_free (&swr);
  av_frame_free (&frame);
  av_packet_free (&pkt);
  avcodec_free_context (&ctx);
  avformat_close_input (&fmt);

  g_assert_nonnull (buf);
  out->data = buf;
  out->frames = n;
}

static void
float_audio_clear (FloatAudio *a)
{
  g_free (a->data);
  a->data = NULL;
  a->frames = 0;
}

/* Mean absolute amplitude over [start_us, end_us). */
static gdouble
window_mean_abs (const FloatAudio *a, gint64 start_us, gint64 end_us)
{
  gint64 s = start_us * 48 / 1000;
  gint64 e = end_us * 48 / 1000;

  g_assert_cmpint ((int) s, <, (int) a->frames);

  e = MIN (e, a->frames);

  gdouble acc = 0;
  gint64 count = 0;

  for (gint64 i = s; i < e; i++)
    {
      acc += fabs (a->data[i * 2]);
      acc += fabs (a->data[i * 2 + 1]);
      count += 2;
    }

  return acc / (gdouble) MAX (count, 1);
}

/* Mean signed amplitude of ONE channel over [start_us, end_us). The
 * pan proofs compare channels against each other, so the signed sum
 * keeps the DC wav's polarity instead of folding both channels. */
static gdouble
channel_mean_signed (const FloatAudio *a, gint64 start_us, gint64 end_us, int channel)
{
  gint64 s = start_us * 48 / 1000;
  gint64 e = end_us * 48 / 1000;

  g_assert_cmpint ((int) s, <, (int) a->frames);

  e = MIN (e, a->frames);

  gdouble acc = 0;
  gint64 count = 0;

  for (gint64 i = s; i < e; i++)
    {
      acc += a->data[i * 2 + channel];
      count++;
    }

  return acc / (gdouble) MAX (count, 1);
}

/* No .oe-export-* temp file may survive a finished run. */
static void
assert_no_temp_files (const OeFixtures *fx)
{
  GDir *dir = g_dir_open (fx->dir, 0, NULL);

  g_assert_nonnull (dir);

  const gchar *name;

  while ((name = g_dir_read_name (dir)) != NULL)
    g_assert_false (g_str_has_prefix (name, ".oe-export-"));

  g_dir_unref (dir);
}

static gboolean
file_has_prefix (const gchar *path, const gchar *prefix, gsize prefix_len)
{
  gchar *contents = NULL;
  gsize len = 0;

  if (!g_file_get_contents (path, &contents, &len, NULL))
    return FALSE;

  const gboolean ok = len >= prefix_len && memcmp (contents, prefix, prefix_len) == 0;

  g_free (contents);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void
test_frame_grid (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  /* One dummy media and two half-second clips on a video track: the
   * grid needs only the model, no decodable content. */
  OeProject *project = new_project_25fps ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint dummy = add_media (project, fx->txt_path);

  insert_clip (project, video, dummy, 0, 500000);
  insert_clip (project, video, dummy, 500000, 500000);

  OeSequence seq = { 0 }; /* zeroed: get_sequence overwrites wholesale */

  oe_project_get_sequence (project, &seq);

  /* End 1 s / 40 ms per frame → ceil = 25 frames; frame f starts at
   * f * 40 ms. */
  g_assert_cmpint ((int) oe_export_frame_count (&seq), ==, 25);
  g_assert_cmpint ((int) oe_export_frame_time_us (0, &seq), ==, 0);
  g_assert_cmpint ((int) oe_export_frame_time_us (1, &seq), ==, (int) TEST_FRAME_US);
  g_assert_cmpint ((int) oe_export_frame_time_us (24, &seq), ==, 24 * (int) TEST_FRAME_US);

  oe_sequence_clear (&seq);
  g_object_unref (project);
}

static void
test_parity_straight_cut (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);

  OeSequence seq = { 0 }; /* zeroed: get_sequence overwrites wholesale */

  oe_project_get_sequence (project, &seq);

  OeRenderSource source = { &seq, test_resolve, project };

  /* Before the cut: red dominates. The half-open rule puts 500 ms on
   * the second clip: position == clip start belongs to the new clip. */
  GError *error = NULL;
  guint8 *frame = oe_render_frame_at (&source, 400000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);
  assert_dominant (frame, TEST_W, TEST_H, "pre-cut is red", 0xe0, 0x20, 0x20);
  g_free (frame);

  /* After the cut: blue dominates. */
  frame = oe_render_frame_at (&source, 600000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);
  assert_dominant (frame, TEST_W, TEST_H, "post-cut is blue", 0x20, 0x30, 0xe0);
  g_free (frame);

  /* Exactly at the cut: the topmost covering clip wins → blue. */
  frame = oe_render_frame_at (&source, 500000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);
  assert_dominant (frame, TEST_W, TEST_H, "at the cut is blue", 0x20, 0x30, 0xe0);
  g_free (frame);

  oe_sequence_clear (&seq);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
}

static void
test_container_truth (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);

  /* Audio so the container carries both streams. */
  write_dc_wav (fx, "tone.wav", 8192, 500000);
  gchar *wav = g_build_filename (fx->dir, "tone.wav", NULL);
  const guint audio = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint wav_ref = add_media (project, wav);

  insert_clip (project, audio, wav_ref, 0, 500000);

  gchar *dest = g_build_filename (fx->dir, "out.mp4", NULL);
  GError *error = NULL;

  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  OeProbeInfo info;

  oe_probe_info_init (&info);
  g_assert_true (oe_probe_file (dest, &info, &error));
  g_assert_no_error (error);

  /* FFmpeg names the MP4 demuxer by its full alias list. */
  g_assert_nonnull (strstr (info.container_name, "mp4"));
  g_assert_cmpstr (info.video_codec, ==, "h264");
  g_assert_cmpstr (info.audio_codec, ==, "aac");
  g_assert_cmpint (info.width, ==, TEST_W);
  g_assert_cmpint (info.height, ==, TEST_H);

  /* Duration within one frame of the ceil grid: 25 frames = 1 s. */
  const gint64 expected = 25 * TEST_FRAME_US;

  g_assert_cmpint ((int) info.duration_us, >=, (int) expected - (int) TEST_FRAME_US);
  g_assert_cmpint ((int) info.duration_us, <=, (int) expected + (int) TEST_FRAME_US);

  oe_probe_info_clear (&info);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (wav);
  g_free (dest);
}

static void
test_content_round_trip (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);

  write_dc_wav (fx, "tone.wav", 8192, 500000);
  gchar *wav = g_build_filename (fx->dir, "tone.wav", NULL);
  const guint audio = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint wav_ref = add_media (project, wav);

  insert_clip (project, audio, wav_ref, 0, 500000);

  gchar *dest = g_build_filename (fx->dir, "out.mp4", NULL);
  GError *error = NULL;

  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  /* Video: frame 0 sits in the red clip; the last frame sits in the
   * blue one — the exported file preserves the cut. The final frame is
   * decoded "to end of stream" rather than by hard index: this FFmpeg
   * build's h264 decoder does not emit the final access unit of x264
   * streams whose closing GOP was shortened by a scenecut (all 25
   * frames are present in the container — /export/container-truth
   * checks the full-grid duration) — see the bug log in
   * docs/learning/phase-8.md. */
  int w = 0, h = 0;
  guint8 *frame = decode_video_frame_bgra (dest, 0, &w, &h);

  g_assert_cmpint (w, ==, TEST_W);
  g_assert_cmpint (h, ==, TEST_H);
  assert_dominant (frame, w, h, "exported frame 0 is red", 0xe0, 0x20, 0x20);
  g_free (frame);

  frame = decode_last_video_frame_bgra (dest, &w, &h);

  assert_dominant (frame, w, h, "exported last frame is blue", 0x20, 0x30, 0xe0);
  g_free (frame);

  /* Audio: the DC clip fills [0, 500 ms) at 8192/32768 ≈ 0.25; after
   * the clip the AAC track is silent. AAC's psychoacoustic filter
   * attenuates pure DC (measured ≈ 0.7x here), so the band is wide. */
  FloatAudio audio_back;

  decode_audio_float (dest, &audio_back);

  const gdouble in_clip = window_mean_abs (&audio_back, 100000, 400000);

  g_assert_cmpfloat (in_clip, >, 0.12);
  g_assert_cmpfloat (in_clip, <, 0.35);

  const gdouble gap = window_mean_abs (&audio_back, 600000, 900000);

  g_assert_cmpfloat (gap, <, 0.03);

  float_audio_clear (&audio_back);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (wav);
  g_free (dest);
}

static void
test_mixdown_sums (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);

  /* Two DC sources at 0.25 overlapping [250, 500) ms on separate
   * tracks: the mix must sum to 0.5 there, 0.25 in the single-clip
   * spans, and stay silent in [750, 1000) — the video clip keeps the
   * sequence 1 s long so the gap window exists. */
  write_dc_wav (fx, "a.wav", 8192, 500000);
  write_dc_wav (fx, "b.wav", 8192, 500000);

  gchar *a = g_build_filename (fx->dir, "a.wav", NULL);
  gchar *b = g_build_filename (fx->dir, "b.wav", NULL);
  const guint t1 = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint t2 = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint ref_a = add_media (project, a);
  const guint ref_b = add_media (project, b);

  insert_clip (project, t1, ref_a, 0, 500000);
  insert_clip (project, t2, ref_b, 250000, 500000);

  gchar *dest = g_build_filename (fx->dir, "out.mp4", NULL);
  GError *error = NULL;

  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  FloatAudio mix;

  decode_audio_float (dest, &mix);

  const gdouble a_only = window_mean_abs (&mix, 50000, 200000);
  const gdouble overlap = window_mean_abs (&mix, 300000, 450000);
  const gdouble b_only = window_mean_abs (&mix, 550000, 700000);
  const gdouble gap = window_mean_abs (&mix, 800000, 950000);

  /* AAC attenuates DC (see /export/content-round-trip), so the summation
   * proof is a ratio: the overlap span must clearly exceed either single
   * clip span, not an absolute encoder-derived level. */
  g_assert_cmpfloat (a_only, >, 0.12);
  g_assert_cmpfloat (b_only, >, 0.12);
  g_assert_cmpfloat (overlap, >, 1.4 * a_only);
  g_assert_cmpfloat (overlap, >, 1.4 * b_only);
  g_assert_cmpfloat (gap, <, 0.03);

  float_audio_clear (&mix);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (a);
  g_free (b);
  g_free (dest);
}

typedef struct
{
  gint after; /* cancel once progress reports this many frames */
  gint seen;
} CancelGate;

static void
gate_progress (gint64 frame_index, gint64 total_frames G_GNUC_UNUSED, gpointer user_data)
{
  CancelGate *gate = user_data;

  gate->seen = (gint) frame_index;
}

static gboolean
gate_cancel (gpointer user_data)
{
  const CancelGate *gate = user_data;

  return gate->seen >= gate->after;
}

static void
test_cancellation (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);
  gchar *dest = g_build_filename (fx->dir, "out.mp4", NULL);

  /* The destination must not exist after a cancelled export. */
  CancelGate gate = { .after = 5, .seen = 0 };
  GError *error = NULL;

  g_assert_false (run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, gate_cancel, &gate,
                              gate_progress, &gate, &error));
  g_assert_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_CANCELLED);
  g_clear_error (&error);

  g_assert_false (g_file_test (dest, G_FILE_TEST_EXISTS));
  assert_no_temp_files (fx);

  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (dest);
}

static void
test_atomic_failure (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);
  const guint audio = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint garbage = add_media (project, fx->txt_path);

  /* A clip whose source cannot be decoded: the mixdown fails after
   * the temp file and the MP4 header already exist. */
  insert_clip (project, audio, garbage, 0, 500000);

  gchar *dest = g_build_filename (fx->dir, "out.mp4", NULL);
  const gchar *original = "ORIGINAL-DESTINATION-BYTES";

  g_assert_true (g_file_set_contents (dest, original, -1, NULL));
  g_assert_true (file_has_prefix (dest, original, strlen (original)));

  GError *error = NULL;

  g_assert_false (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_FAILED);
  g_clear_error (&error);

  /* The pre-existing destination is byte-identical; no temp remains. */
  g_assert_true (file_has_prefix (dest, original, strlen (original)));
  assert_no_temp_files (fx);

  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (dest);
}

static void
test_atomic_failure_unwritable_dir (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  if (geteuid () == 0)
    {
      g_test_skip ("running as root: mode bits do not block file creation");
      return;
    }

  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);

  gchar *ro_dir = g_build_filename (fx->dir, "readonly", NULL);

  g_assert_true (g_mkdir (ro_dir, 0755) == 0);
  g_assert_true (g_chmod (ro_dir, 0500) == 0); /* r-x: no create, no write */

  gchar *dest = g_build_filename (ro_dir, "out.mp4", NULL);
  GError *error = NULL;

  g_assert_false (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_error (error, OE_EXPORT_ERROR, OE_EXPORT_ERROR_OPEN_FAILED);
  g_clear_error (&error);

  g_assert_false (g_file_test (dest, G_FILE_TEST_EXISTS));

  g_chmod (ro_dir, 0700); /* restore so fixture teardown can remove it */
  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (ro_dir);
  g_free (dest);
}

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

/* Channel means over a rectangle — the window-mean idiom. */
static void
window_mean (const guint8 *bgra, int w, int h, int x0, int y0, int x1, int y1, double out[3])
{
  glong r = 0, g = 0, b = 0;

  g_assert_cmpint (x1, <=, w);
  g_assert_cmpint (y1, <=, h);

  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++)
      {
        const guint8 *px = bgra + ((gsize) y * w + x) * 4;

        r += px[2];
        g += px[1];
        b += px[0];
      }

  const int n = (x1 - x0) * (y1 - y0);

  out[0] = (double) r / n;
  out[1] = (double) g / n;
  out[2] = (double) b / n;
}

/* Channel dominance over a rectangle: assert_dominant restricted to a
 * sub-window, for per-region seam verdicts. */
static void
assert_rect_dominant (const guint8 *bgra, int w, int h, int x0, int y0, int x1, int y1,
                      const gchar *what, guint8 r, guint8 g, guint8 b)
{
  double mean[3];

  window_mean (bgra, w, h, x0, y0, x1, y1, mean);
  g_assert_cmpstr (what, !=, NULL);

  if (r >= g && r >= b)
    {
      g_assert_cmpfloat (mean[0], >, mean[1]);
      g_assert_cmpfloat (mean[0], >, mean[2]);
    }
  else if (g >= r && g >= b)
    {
      g_assert_cmpfloat (mean[1], >, mean[0]);
      g_assert_cmpfloat (mean[1], >, mean[2]);
    }
  else
    {
      g_assert_cmpfloat (mean[2], >, mean[0]);
      g_assert_cmpfloat (mean[2], >, mean[1]);
    }
}

/* /export/blend-unit: the pure channel blend is straight integer
 * src-over and stays within ±1 of the exact alpha formula. */
static void
test_blend_unit (void)
{
  static const struct
  {
    guint8 dst, src, a;
  } cases[] = {
    { 0x20, 0xe0, 128 },                  /* red over blue, half opacity: the spec case */
    { 0xe0, 0x20, 128 }, { 0, 255, 255 }, /* full opacity: passthrough of the source */
    { 255, 0, 0 },                        /* zero opacity: passthrough of the backdrop */
    { 0, 255, 1 },                        /* one step of transparency */
    { 255, 0, 254 },     { 10, 245, 64 }, { 245, 10, 64 }, { 128, 128, 128 },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      const double exact = (cases[i].src * cases[i].a + cases[i].dst * (255 - cases[i].a)) / 255.0;
      const int got = oe_render_blend_channel (cases[i].dst, cases[i].src, cases[i].a);

      g_assert_cmpint (got, >=, (int) exact - 1);
      g_assert_cmpint (got, <=, (int) exact + 1);
      g_assert_cmpint (got, >=, 0);
      g_assert_cmpint (got, <=, 255);
    }

  /* Monotone in alpha for fixed endpoints: more opacity moves the
   * result toward the source, never past it. */
  int prev = 0;

  for (int a = 0; a <= 255; a++)
    {
      const int got = oe_render_blend_channel (0, 255, (guint8) a);

      g_assert_cmpint (got, >=, prev);
      prev = got;
    }
}

/* Two video tracks over the same time window: red beneath green. */
static OeProject *
build_two_layer_video (const OeFixtures *fx, gchar **red_path, gchar **green_path)
{
  write_solid_avi (fx, "red.avi", 0xe0, 0x20, 0x20, TEST_FPS / 2);
  write_solid_avi (fx, "green.avi", 0x20, 0xe0, 0x20, TEST_FPS / 2);

  *red_path = g_build_filename (fx->dir, "red.avi", NULL);
  *green_path = g_build_filename (fx->dir, "green.avi", NULL);

  OeProject *project = new_project_25fps ();
  const guint red_track = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint green_track = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint red = add_media (project, *red_path);
  const guint green = add_media (project, *green_path);

  insert_clip (project, red_track, red, 0, 480000);
  insert_clip (project, green_track, green, 0, 480000);
  return project;
}

/* /export/two-layer-seam: track order decides the layering; a
 * transformed upper clip wins only inside its footprint; opacity-0
 * layers skip out; half opacity blends to the exact src-over value. */
static void
test_two_layer_seam (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *green;
  OeProject *project = build_two_layer_video (fx, &red, &green);

  OeSequence seq = { 0 }; /* zeroed: get_sequence overwrites wholesale */

  oe_project_get_sequence (project, &seq);

  OeRenderSource source = { &seq, test_resolve, project };
  GError *error = NULL;

  /* Default transforms: the topmost covering clip wins everywhere. */
  guint8 *frame = oe_render_frame_at (&source, 240000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);
  assert_dominant (frame, TEST_W, TEST_H, "topmost wins under default transforms", 0x20, 0xe0,
                   0x20);
  g_free (frame);

  /* A half-frame upper clip (scale 500, centered): green inside its
   * bounds, fallthrough to the lower track outside them. Each visual
   * edit must be re-snapshotted into the render source — the sequence
   * copy is deep, like the monitor's session snapshot. */
  OeClipVisual v = oe_clip_visual_identity ();

  v.scale_permille = 500;
  g_assert_true (oe_project_set_clip_visual (project, 1, 0, &v, NULL));
  oe_sequence_clear (&seq);
  oe_project_get_sequence (project, &seq);

  frame = oe_render_frame_at (&source, 240000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);
  assert_rect_dominant (frame, TEST_W, TEST_H, 60, 40, 100, 80, "inside the scaled clip is green",
                        0x20, 0xe0, 0x20);
  assert_rect_dominant (frame, TEST_W, TEST_H, 4, 4, 20, 20, "outside it falls through to red",
                        0xe0, 0x20, 0x20);
  g_free (frame);

  /* Opacity 0 skips the layer out of the blend entirely. */
  v.scale_permille = 1000;
  v.opacity = 0;
  g_assert_true (oe_project_set_clip_visual (project, 1, 0, &v, NULL));
  oe_sequence_clear (&seq);
  oe_project_get_sequence (project, &seq);

  frame = oe_render_frame_at (&source, 240000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);
  assert_dominant (frame, TEST_W, TEST_H, "opacity-0 layer skips out", 0xe0, 0x20, 0x20);
  g_free (frame);

  /* Half opacity: the interior blends green over red to ≈ (128, 128, 32). */
  v.opacity = 128;
  g_assert_true (oe_project_set_clip_visual (project, 1, 0, &v, NULL));
  oe_sequence_clear (&seq);
  oe_project_get_sequence (project, &seq);

  frame = oe_render_frame_at (&source, 240000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);

  double mean[3];

  window_mean (frame, TEST_W, TEST_H, 60, 40, 100, 80, mean);
  g_assert_cmpfloat (fabs (mean[0] - 128.0), <=, 4.0);
  g_assert_cmpfloat (fabs (mean[1] - 128.0), <=, 4.0);
  g_assert_cmpfloat (fabs (mean[2] - 32.0), <=, 4.0);
  g_free (frame);

  oe_sequence_clear (&seq);
  g_object_unref (project);
  g_free (red);
  g_free (green);
}

/* /export/parity-two-layer: the exported file decodes back to what the
 * shared seam renders — one compositor, equal canvas sizes, medium
 * preset. yuv420p chroma subsampling plus x264 quantization make
 * per-pixel equality impossible; 8x8 block means bound |Δ| ≤ 8 per
 * channel (the documented export tolerance) while the dominant-color
 * class must agree with the seam exactly. */
static void
test_parity_two_layer (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *green;
  OeProject *project = build_two_layer_video (fx, &red, &green);

  /* A transformed, translucent upper clip: geometry, blending, and
   * fallthrough all in one frame. */
  OeClipVisual v = oe_clip_visual_identity ();

  v.scale_permille = 625;
  v.pos_x = 20;
  v.pos_y = 10;
  v.opacity = 200;
  g_assert_true (oe_project_set_clip_visual (project, 1, 0, &v, NULL));

  OeSequence seq = { 0 }; /* zeroed: get_sequence overwrites wholesale */

  oe_project_get_sequence (project, &seq);

  OeRenderSource source = { &seq, test_resolve, project };

  /* What the seam renders at the mid-clip instant. */
  GError *error = NULL;
  guint8 *rendered = oe_render_frame_at (&source, 240000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (rendered);

  /* What the export put in the file at the same instant (frame 6 of
   * 25 fps = 240 ms, mid-stream decoded like the other parity tests). */
  gchar *dest = g_build_filename (fx->dir, "out.mp4", NULL);

  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  int w = 0, h = 0;
  guint8 *decoded = decode_video_frame_bgra (dest, 6, &w, &h);

  g_assert_nonnull (decoded);
  g_assert_cmpint (w, ==, TEST_W);
  g_assert_cmpint (h, ==, TEST_H);

  /* Dominant class agrees per region through the seam. */
  assert_rect_dominant (rendered, TEST_W, TEST_H, 40, 30, 100, 70, "render center is green", 0x20,
                        0xe0, 0x20);
  assert_rect_dominant (decoded, TEST_W, TEST_H, 40, 30, 100, 70, "export center is green", 0x20,
                        0xe0, 0x20);
  assert_rect_dominant (rendered, TEST_W, TEST_H, 4, 4, 16, 16, "render corner is red", 0xe0, 0x20,
                        0x20);
  assert_rect_dominant (decoded, TEST_W, TEST_H, 4, 4, 16, 16, "export corner is red", 0xe0, 0x20,
                        0x20);

  /* Block-mean parity: |Δ| ≤ 8 per channel in every 8x8 block. */
  const int BS = 8;

  for (int by = 0; by < TEST_H / BS; by++)
    for (int bx = 0; bx < TEST_W / BS; bx++)
      {
        double rm[3], dm[3];

        window_mean (rendered, TEST_W, TEST_H, bx * BS, by * BS, (bx + 1) * BS, (by + 1) * BS, rm);
        window_mean (decoded, TEST_W, TEST_H, bx * BS, by * BS, (bx + 1) * BS, (by + 1) * BS, dm);
        for (int c = 0; c < 3; c++)
          g_assert_cmpfloat (fabs (rm[c] - dm[c]), <=, 8.0);
      }

  g_free (rendered);
  g_free (decoded);
  g_free (dest);
  oe_sequence_clear (&seq);
  g_object_unref (project);
  g_free (red);
  g_free (green);
}

/* ------------------------------------------------------------------ */
/* Wave B: transitions, fades, acceptance parity                        */
/* ------------------------------------------------------------------ */

/* Solid-color layers with a boundary transition between the two lower
 * clips and a keyed, translucent green layer on top. */
static OeProject *
build_transition_project (const OeFixtures *fx, gchar **red_path, gchar **blue_path)
{
  write_solid_avi (fx, "red.avi", 0xe0, 0x20, 0x20, TEST_FPS);
  write_solid_avi (fx, "blue.avi", 0x20, 0x30, 0xe0, TEST_FPS);

  *red_path = g_build_filename (fx->dir, "red.avi", NULL);
  *blue_path = g_build_filename (fx->dir, "blue.avi", NULL);

  OeProject *project = new_project_25fps ();
  const guint lower = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint upper = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint red = add_media (project, *red_path);
  const guint blue = add_media (project, *blue_path);

  insert_clip (project, lower, red, 0, 480000);
  insert_clip (project, lower, blue, 480000, 480000);
  insert_clip (project, upper, red, 0, 480000);

  /* The boundary transition, then the keyed upper layer: opacity ramps
   * clip-relatively 255 → 0 across [0, 480 ms), so the layer fades
   * away exactly where the transition window opens. */
  OeTransition t = { 0, 480000, 100000, OE_TRANSITION_CROSS_DISSOLVE };

  g_assert_true (oe_project_add_transition (project, &t, NULL));

  OeClipVisual v = oe_clip_visual_identity ();

  g_assert_true (oe_project_set_clip_visual (project, upper, 0, &v, NULL));
  g_assert_true (
      oe_project_set_clip_keyframe (project, upper, 0, OE_KEYFRAME_OPACITY, 0, 255, NULL));
  g_assert_true (
      oe_project_set_clip_keyframe (project, upper, 0, OE_KEYFRAME_OPACITY, 480000, 0, NULL));
  return project;
}

/* /export/transition-blend: the compositor's two-input ramp — w=0/255
 * degenerate to the straight cut, the midpoint mixes 50/50, and
 * dip-to-black sinks to half-dark at the midpoint. */
static void
test_transition_blend (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_transition_project (fx, &red, &blue);

  OeSequence seq = { 0 }; /* zeroed: get_sequence overwrites wholesale */

  oe_project_get_sequence (project, &seq);

  OeRenderSource source = { &seq, test_resolve, project };
  GError *error = NULL;
  double mean[3];
  guint8 *frame;

  /* Window edges are the documented degenerate points: the whole
   * frame is one clip's picture, straight cut, nothing blended. */
  frame = oe_render_frame_at (&source, 430000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);
  window_mean (frame, TEST_W, TEST_H, 0, 0, TEST_W, TEST_H, mean);
  g_assert_cmpfloat (fabs (mean[0] - 0xe0), <=, 2.0);
  g_assert_cmpfloat (fabs (mean[1] - 0x20), <=, 2.0);
  g_assert_cmpfloat (fabs (mean[2] - 0x20), <=, 2.0);
  g_free (frame);

  frame = oe_render_frame_at (&source, 530000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);
  window_mean (frame, TEST_W, TEST_H, 0, 0, TEST_W, TEST_H, mean);
  g_assert_cmpfloat (fabs (mean[0] - 0x20), <=, 2.0);
  g_assert_cmpfloat (fabs (mean[1] - 0x30), <=, 2.0);
  g_assert_cmpfloat (fabs (mean[2] - 0xe0), <=, 2.0);
  g_free (frame);

  /* Midpoint of the cross-dissolve (ramp 128): (red*(255-w)+blue*w)/255
   * per channel; the keyed upper layer samples to opacity 128 at 240 ms
   * and is gone by 480 ms. The lower pair dominates: ≈ (128, 40, 128)
   * with the translucent red on top nudging channels a few counts. */
  frame = oe_render_frame_at (&source, 480000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);
  window_mean (frame, TEST_W, TEST_H, 0, 0, TEST_W, TEST_H, mean);
  g_assert_cmpfloat (mean[0], >, 100.0);
  g_assert_cmpfloat (mean[0], <, 150.0);
  g_assert_cmpfloat (mean[1], <, 70.0);
  g_assert_cmpfloat (mean[2], >, 100.0);
  g_free (frame);

  oe_sequence_clear (&seq);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
}

/* /export/mixdown-fade-ratio: the shared envelope shapes the exported
 * mixdown — windowed-mean amplitude inside the ramps sits at a fraction
 * of the steady span, in the same AAC-tolerant ratio-band idiom as the
 * summation test (encoder DC attenuation cancels in ratios). */
static void
test_mixdown_fade_ratio (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);

  /* One DC clip [0, 600 ms) with 250 ms ramps on both sides: gain is
   * min(1024, round(t·1024/250000), round((600000−t)·1024/250000)) —
   * ≈ 0.48 of full across the sampled ramp windows, full mid-clip. */
  write_dc_wav (fx, "tone.wav", 8192, 600000);
  gchar *wav = g_build_filename (fx->dir, "tone.wav", NULL);
  const guint audio = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint ref = add_media (project, wav);

  insert_clip (project, audio, ref, 0, 600000);

  OeClip clip;

  g_assert_true (oe_project_get_clip (project, audio, 0, &clip));
  OeClipVisual v = clip.visual;

  v.fade_in_us = 250000;
  v.fade_out_us = 250000;
  g_assert_true (oe_project_set_clip_visual (project, audio, 0, &v, NULL));

  gchar *dest = g_build_filename (fx->dir, "out.mp4", NULL);
  GError *error = NULL;

  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  FloatAudio mix;

  decode_audio_float (dest, &mix);

  const gdouble ramp_in = window_mean_abs (&mix, 60000, 180000);
  const gdouble steady = window_mean_abs (&mix, 270000, 330000);
  const gdouble ramp_out = window_mean_abs (&mix, 420000, 540000);

  /* The steady span carries the full level; each ramp window sits at
   * its envelope mean (≈ 0.48), bounded loosely for AAC's DC filter. */
  g_assert_cmpfloat (steady, >, 0.12);
  g_assert_cmpfloat (ramp_in, >, 0.25 * steady);
  g_assert_cmpfloat (ramp_in, <, 0.75 * steady);
  g_assert_cmpfloat (ramp_out, >, 0.25 * steady);
  g_assert_cmpfloat (ramp_out, <, 0.75 * steady);

  float_audio_clear (&mix);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (wav);
  g_free (dest);
}

/* /export/mixdown-pan-positions: the clip and track pan pairs steer
 * the exported mix per channel — a hard pan sends one channel's
 * windowed mean far above the other's (ratio-band idiom: encoder DC
 * attenuation cancels in ratios), center keeps the channels matched. */
static void
test_mixdown_pan_positions (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);

  write_dc_wav (fx, "tone.wav", 8192, 500000);
  gchar *wav = g_build_filename (fx->dir, "tone.wav", NULL);
  const guint audio = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint ref = add_media (project, wav);
  GError *error = NULL;

  /* Clip pan hard-left: the clip pair folds the right channel away. */
  insert_clip (project, audio, ref, 0, 500000);
  OeClipAudio ca = { .gain = 1024, .pan = 0 };

  g_assert_true (oe_project_set_clip_audio (project, audio, 0, &ca, NULL));

  gchar *dest = g_build_filename (fx->dir, "left.mp4", NULL);

  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  FloatAudio mix;

  decode_audio_float (dest, &mix);

  const gdouble hard_l = channel_mean_signed (&mix, 100000, 400000, 0);
  const gdouble hard_r = channel_mean_signed (&mix, 100000, 400000, 1);

  g_assert_cmpfloat (hard_l, >, 0.12);
  g_assert_cmpfloat (hard_l, >, 10.0 * fabs (hard_r));

  float_audio_clear (&mix);
  g_free (dest);

  /* Clip pan center (the identity state): the channels match. */
  ca.pan = 512;
  g_assert_true (oe_project_set_clip_audio (project, audio, 0, &ca, NULL));

  dest = g_build_filename (fx->dir, "center.mp4", NULL);
  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  decode_audio_float (dest, &mix);

  const gdouble center_l = channel_mean_signed (&mix, 100000, 400000, 0);
  const gdouble center_r = channel_mean_signed (&mix, 100000, 400000, 1);

  g_assert_cmpfloat (center_l, >, 0.12);
  g_assert_cmpfloat (fabs (center_l - center_r), <=, 0.15 * fabs (center_l));

  float_audio_clear (&mix);
  g_free (dest);

  /* Track pan hard-right: the track pair folds the left channel —
   * the track half of the chain folds the OTHER way from the clip
   * probe above. */
  OeTrackAudio ta = { .volume = 1024, .pan = 1024, .mute = 0, .solo = 0 };

  g_assert_true (oe_project_set_track_audio (project, audio, &ta, NULL));

  dest = g_build_filename (fx->dir, "right.mp4", NULL);
  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  decode_audio_float (dest, &mix);

  const gdouble track_l = channel_mean_signed (&mix, 100000, 400000, 0);
  const gdouble track_r = channel_mean_signed (&mix, 100000, 400000, 1);

  g_assert_cmpfloat (track_r, >, 0.12);
  g_assert_cmpfloat (track_r, >, 10.0 * fabs (track_l));

  float_audio_clear (&mix);
  g_free (dest);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (wav);
}

/* /export/mixdown-gain-ratio: clip gain and track volume halve the
 * exported windowed mean — fixed-point 512 halves the chain exactly,
 * ratio-banded for AAC's level-dependent DC filter. */
static void
test_mixdown_gain_ratio (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);

  write_dc_wav (fx, "tone.wav", 8192, 500000);
  gchar *wav = g_build_filename (fx->dir, "tone.wav", NULL);
  const guint audio = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint ref = add_media (project, wav);
  GError *error = NULL;

  /* Baseline: identity audio everywhere. */
  insert_clip (project, audio, ref, 0, 500000);

  gchar *dest = g_build_filename (fx->dir, "base.mp4", NULL);

  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  FloatAudio mix;

  decode_audio_float (dest, &mix);

  const gdouble baseline = window_mean_abs (&mix, 100000, 400000);

  g_assert_cmpfloat (baseline, >, 0.12);

  float_audio_clear (&mix);
  g_free (dest);

  /* Clip gain 512 (half of unity): the mix halves. */
  OeClipAudio ca = { .gain = 512, .pan = 512 };

  g_assert_true (oe_project_set_clip_audio (project, audio, 0, &ca, NULL));

  dest = g_build_filename (fx->dir, "half-clip.mp4", NULL);
  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  decode_audio_float (dest, &mix);

  const gdouble clip_gain = window_mean_abs (&mix, 100000, 400000);

  g_assert_cmpfloat (clip_gain, >, 0.35 * baseline);
  g_assert_cmpfloat (clip_gain, <, 0.65 * baseline);

  float_audio_clear (&mix);
  g_free (dest);

  /* Track volume 512 with the clip at identity: the mix halves too. */
  ca.gain = 1024;
  g_assert_true (oe_project_set_clip_audio (project, audio, 0, &ca, NULL));

  OeTrackAudio ta = { .volume = 512, .pan = 512, .mute = 0, .solo = 0 };

  g_assert_true (oe_project_set_track_audio (project, audio, &ta, NULL));

  dest = g_build_filename (fx->dir, "half-track.mp4", NULL);
  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  decode_audio_float (dest, &mix);

  const gdouble track_gain = window_mean_abs (&mix, 100000, 400000);

  g_assert_cmpfloat (track_gain, >, 0.35 * baseline);
  g_assert_cmpfloat (track_gain, <, 0.65 * baseline);

  float_audio_clear (&mix);
  g_free (dest);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (wav);
}

/* /export/mixdown-mute-solo-matrix: the serialized D5 matrix acts on
 * the export path exactly as preview will — mute zeroes a track, any
 * soloed track silences every non-soloed one, and clearing the
 * matrix restores ordinary summation. */
static void
test_mixdown_mute_solo_matrix (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);

  /* Two DC sources overlapping [250, 500) ms on separate audio
   * tracks — the /export/mixdown-sums geometry. */
  write_dc_wav (fx, "a.wav", 8192, 500000);
  write_dc_wav (fx, "b.wav", 8192, 500000);

  gchar *a = g_build_filename (fx->dir, "a.wav", NULL);
  gchar *b = g_build_filename (fx->dir, "b.wav", NULL);
  const guint t1 = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint t2 = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint ref_a = add_media (project, a);
  const guint ref_b = add_media (project, b);

  insert_clip (project, t1, ref_a, 0, 500000);
  insert_clip (project, t2, ref_b, 250000, 500000);

  GError *error = NULL;

  /* Mute T1: only T2 survives anywhere. */
  OeTrackAudio mute = { .volume = 1024, .pan = 512, .mute = 1, .solo = 0 };

  g_assert_true (oe_project_set_track_audio (project, t1, &mute, NULL));

  gchar *dest = g_build_filename (fx->dir, "muted.mp4", NULL);

  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  FloatAudio mix;

  decode_audio_float (dest, &mix);

  const gdouble muted_b = window_mean_abs (&mix, 550000, 700000);
  const gdouble muted_a = window_mean_abs (&mix, 50000, 200000);

  g_assert_cmpfloat (muted_b, >, 0.12);
  g_assert_cmpfloat (muted_a, <, 0.25 * muted_b);

  float_audio_clear (&mix);
  g_free (dest);

  /* Solo T1 (mute cleared): solo overrides — T2 is silenced even
   * unmuted. */
  OeTrackAudio solo = { .volume = 1024, .pan = 512, .mute = 0, .solo = 1 };

  g_assert_true (oe_project_set_track_audio (project, t1, &solo, NULL));

  dest = g_build_filename (fx->dir, "solo.mp4", NULL);
  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  decode_audio_float (dest, &mix);

  const gdouble solo_a = window_mean_abs (&mix, 50000, 200000);
  const gdouble solo_b = window_mean_abs (&mix, 550000, 700000);

  g_assert_cmpfloat (solo_a, >, 0.12);
  g_assert_cmpfloat (solo_b, <, 0.25 * solo_a);

  float_audio_clear (&mix);
  g_free (dest);

  /* Clear the matrix: ordinary summation returns (both audible). */
  OeTrackAudio open_ = { .volume = 1024, .pan = 512, .mute = 0, .solo = 0 };

  g_assert_true (oe_project_set_track_audio (project, t1, &open_, NULL));

  dest = g_build_filename (fx->dir, "open.mp4", NULL);
  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  decode_audio_float (dest, &mix);

  const gdouble open_a = window_mean_abs (&mix, 50000, 200000);
  const gdouble open_b = window_mean_abs (&mix, 550000, 700000);
  const gdouble open_overlap = window_mean_abs (&mix, 300000, 450000);

  g_assert_cmpfloat (open_a, >, 0.12);
  g_assert_cmpfloat (open_b, >, 0.12);
  g_assert_cmpfloat (open_overlap, >, 1.4 * open_a);
  g_assert_cmpfloat (open_overlap, >, 1.4 * open_b);

  float_audio_clear (&mix);
  g_free (dest);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (a);
  g_free (b);
}

/* /export/parity-transition-fade: the Wave B acceptance sequence — two
 * video layers, a boundary transition, and an audio fade, rendered by
 * the one compositor and exported through the one mixdown. Block-mean
 * parity bounds |Δ| ≤ 8 per channel; the fade rides the same
 * ratio-band idiom. */
static void
test_parity_transition_fade (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *red, *blue;
  OeProject *project = build_transition_project (fx, &red, &blue);

  /* The audio fade of the acceptance sequence. */
  write_dc_wav (fx, "tone.wav", 8192, 500000);
  gchar *wav = g_build_filename (fx->dir, "tone.wav", NULL);
  const guint audio = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint ref = add_media (project, wav);

  insert_clip (project, audio, ref, 0, 480000);

  OeClip clip;

  g_assert_true (oe_project_get_clip (project, audio, 0, &clip));
  OeClipVisual av = clip.visual;

  av.fade_in_us = 240000;
  g_assert_true (oe_project_set_clip_visual (project, audio, 0, &av, NULL));

  OeSequence seq = { 0 }; /* zeroed: get_sequence overwrites wholesale */

  oe_project_get_sequence (project, &seq);

  OeRenderSource source = { &seq, test_resolve, project };
  GError *error = NULL;

  /* What the seam renders at two instants: mid keyframe fade (240 ms,
   * upper opacity 128) and mid transition (480 ms, upper gone). */
  guint8 *rendered_mid = oe_render_frame_at (&source, 240000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (rendered_mid);

  guint8 *rendered_trans = oe_render_frame_at (&source, 480000, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (rendered_trans);

  gchar *dest = g_build_filename (fx->dir, "out.mp4", NULL);

  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  int w = 0, h = 0;
  guint8 *decoded_mid = decode_video_frame_bgra (dest, 6, &w, &h);

  g_assert_nonnull (decoded_mid);
  g_assert_cmpint (w, ==, TEST_W);
  g_assert_cmpint (h, ==, TEST_H);

  guint8 *decoded_trans = decode_video_frame_bgra (dest, 12, &w, &h);

  g_assert_nonnull (decoded_trans);

  /* Block-mean parity at both instants: |Δ| ≤ 8 per channel. */
  const int BS = 8;
  const struct
  {
    guint8 *rendered, *decoded;
  } pairs[] = {
    { rendered_mid, decoded_mid },
    { rendered_trans, decoded_trans },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (pairs); i++)
    for (int by = 0; by < TEST_H / BS; by++)
      for (int bx = 0; bx < TEST_W / BS; bx++)
        {
          double rm[3], dm[3];

          window_mean (pairs[i].rendered, TEST_W, TEST_H, bx * BS, by * BS, (bx + 1) * BS,
                       (by + 1) * BS, rm);
          window_mean (pairs[i].decoded, TEST_W, TEST_H, bx * BS, by * BS, (bx + 1) * BS,
                       (by + 1) * BS, dm);
          for (int c = 0; c < 3; c++)
            g_assert_cmpfloat (fabs (rm[c] - dm[c]), <=, 8.0);
        }

  /* Audio: the fade-in shapes the mixdown (same ratio-band idiom as
   * /export/mixdown-fade-ratio). */
  FloatAudio mix;

  decode_audio_float (dest, &mix);

  const gdouble ramp = window_mean_abs (&mix, 60000, 180000);
  const gdouble steady = window_mean_abs (&mix, 300000, 450000);

  g_assert_cmpfloat (steady, >, 0.12);
  g_assert_cmpfloat (ramp, >, 0.2 * steady);
  g_assert_cmpfloat (ramp, <, 0.8 * steady);

  float_audio_clear (&mix);
  g_free (rendered_mid);
  g_free (rendered_trans);
  g_free (decoded_mid);
  g_free (decoded_trans);
  g_free (dest);
  oe_sequence_clear (&seq);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (wav);
}

/* ------------------------------------------------------------------ */
/* Phase 10 Wave B: multi-track preview/export audio parity (D1).       */
/* ------------------------------------------------------------------ */

/* Mid-frequency sine fixture: AAC's DC filter would bend an absolute
 * level comparison made against a constant-amplitude WAV, but a 440 Hz
 * tone survives the encode essentially intact, so the preview mix and
 * the exported mixdown can be compared level-for-level in matched
 * time windows. */
static void
write_sine_wav (const OeFixtures *fx, const gchar *name, gint16 amp, double freq, gint64 useconds)
{
  const gint64 frames = useconds * 22050 / 1000000;
  const guint32 data_bytes = (guint32) (frames * 2);
  const guint32 riff_size = 36 + data_bytes;
  const guint32 rate = 22050;
  const guint32 fmt_size = 16;
  const guint16 audio_format = 1, channels = 1, bits = 16, block_align = 2;
  const guint32 byte_rate = rate * 2;
  gchar *path = g_build_filename (fx->dir, name, NULL);
  FILE *f = fopen (path, "wb");

  g_assert_nonnull (f);

  fwrite ("RIFF", 1, 4, f);
  fwrite (&riff_size, 4, 1, f);
  fwrite ("WAVE", 1, 4, f);
  fwrite ("fmt ", 1, 4, f);
  fwrite (&fmt_size, 4, 1, f);
  fwrite (&audio_format, 2, 1, f);
  fwrite (&channels, 2, 1, f);
  fwrite (&rate, 4, 1, f);
  fwrite (&byte_rate, 4, 1, f);
  fwrite (&block_align, 2, 1, f);
  fwrite (&bits, 2, 1, f);
  fwrite ("data", 1, 4, f);
  fwrite (&data_bytes, 4, 1, f);

  for (gint64 i = 0; i < frames; i++)
    {
      const double t = (double) i / 22050.0;
      const gint16 s = (gint16) (amp * sin (2.0 * G_PI * freq * t));

      fwrite (&s, 2, 1, f);
    }

  fclose (f);
  g_free (path);
}

/* Mean absolute amplitude of ONE channel over [start_us, end_us).
 * The parity proof compares per-channel LEVELS between two renderings
 * of the same mix, so the absolute value keeps the sine's magnitude
 * instead of a signed sum cancelling to zero over full periods. */
static gdouble
channel_mean_abs (const FloatAudio *a, gint64 start_us, gint64 end_us, int channel)
{
  gint64 s = start_us * 48 / 1000;
  gint64 e = end_us * 48 / 1000;

  g_assert_cmpint ((int) s, <, (int) a->frames);

  e = MIN (e, a->frames);

  gdouble acc = 0;
  gint64 count = 0;

  for (gint64 i = s; i < e; i++)
    {
      acc += fabs (a->data[i * 2 + channel]);
      count++;
    }

  return acc / (gdouble) MAX (count, 1);
}

/* Accumulates the session's mixed spans for off-line comparison — the
 * preview side of the parity contract. The mix observer reports the
 * span's own format, so matched windows convert with the capture's
 * rate rather than assuming the device format. */
typedef struct
{
  gfloat *data;  /* interleaved, capture's format                  */
  gint64 frames; /* frames accumulated                             */
  int channels;
  int rate;
  gint64 start_us; /* sequence time of data[0]                      */
} MixCapture;

static void
collect_mix (OePlaybackSession *session G_GNUC_UNUSED, gint64 seq_start_us, gsize n_frames,
             int channels, int sample_rate, const gfloat *interleaved, gpointer user_data)
{
  MixCapture *cap = user_data;

  if (cap->frames == 0)
    {
      cap->channels = channels;
      cap->rate = sample_rate;
      cap->start_us = seq_start_us;
    }
  else
    {
      /* The mixed spans must be contiguous — a gap would mean the
       * mixer dropped part of the sequence. */
      const gint64 expected
          = cap->start_us + cap->frames * G_GINT64_CONSTANT (1000000) / MAX (cap->rate, 1);

      g_assert_cmpint ((int) llabs (seq_start_us - expected), <=, 1000);
      g_assert_cmpint (channels, ==, cap->channels);
      g_assert_cmpint (sample_rate, ==, cap->rate);
    }

  const gsize samples = n_frames * (gsize) channels;

  cap->data = g_realloc_n (cap->data, (gsize) (cap->frames + (gint64) n_frames) * (gsize) channels,
                           sizeof (gfloat));
  memcpy (cap->data + (gsize) cap->frames * (gsize) channels, interleaved,
          samples * sizeof (gfloat));
  cap->frames += (gint64) n_frames;
}

static void
mix_capture_clear (MixCapture *cap)
{
  g_clear_pointer (&cap->data, g_free);
  cap->frames = 0;
  cap->channels = 0;
  cap->rate = 0;
  cap->start_us = 0;
}

/* Mean absolute amplitude of one capture channel over [start_us,
 * end_us) in sequence time. */
static gdouble
capture_mean_abs (const MixCapture *cap, gint64 start_us, gint64 end_us, int channel)
{
  gint64 s = (start_us - cap->start_us) * cap->rate / G_GINT64_CONSTANT (1000000);
  gint64 e = (end_us - cap->start_us) * cap->rate / G_GINT64_CONSTANT (1000000);

  g_assert_true (s >= 0);
  g_assert_cmpint ((int) s, <, (int) cap->frames);

  e = MIN (e, cap->frames);

  gdouble acc = 0;
  gint64 count = 0;

  for (gint64 i = s; i < e; i++)
    {
      acc += fabs (cap->data[i * cap->channels + channel]);
      count++;
    }

  return acc / (gdouble) MAX (count, 1);
}

/* /export/parity-audio-mix-two-tracks: the D1 acceptance proof. A
 * two-audio-track project — both tracks carrying the same sine clip
 * with distinct clip/track gain and pan states — is played through
 * the REAL session mixer (virtual clock, deterministic deliveries,
 * mix-observer capture) and exported through the mixdown; per-channel
 * windowed mean levels must agree within 0.02 (the audio analogue of
 * the ≤ 8 per-block video idiom: 8/255 ≈ 0.031, tightened for the
 * sine's higher dynamic range). The mixer sums every audio track per
 * chunk period in track-array order; a single-lane playback (the
 * pre-Wave-B topmost-track-wins mapping) captures only track B, so
 * every channel level lands far outside the tolerance — this test
 * fails by construction until D1 lands. */
static gboolean audio_ready = FALSE;

/* Virtual clock for the parity transport: every tick advances the
 * clock by hand, so no timing depends on wall time — only the
 * worker's decode speed does. */
typedef struct
{
  gint64 now_us;
} ParityClock;

static gint64
parity_fake_time (gpointer data)
{
  return ((ParityClock *) data)->now_us;
}

static void
test_parity_audio_mix_two_tracks (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  g_assert_true (audio_ready); /* SDL audio adapter initialized in main */

  gchar *red, *blue;
  OeProject *project = build_two_cut_video (fx, &red, &blue);

  write_sine_wav (fx, "tone.wav", 8192, 440.0, 700000);
  gchar *wav = g_build_filename (fx->dir, "tone.wav", NULL);
  const guint track_a = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint track_b = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint ref = add_media (project, wav);

  g_assert_cmpuint (track_a, !=, track_b);

  /* Both tracks carry the same clip spanning [0, 0.6 s). */
  insert_clip (project, track_a, ref, 0, 600000);
  insert_clip (project, track_b, ref, 0, 600000);

  /* Distinct levels and pans on both chain stages (buffer-constant
   * factors, D1). Per-channel sums relative to the source sample s:
   *   track A: clip center ×1, volume 2048 → L = R = 2.0 × s
   *   track B: clip gain 512 pan 768 (L 0.5, R 1.5 pairs), volume 512
   *            pan 384 (L 1.25, R 0.75 pairs)
   *            → L = 0.15625 × s, R = 0.28125 × s
   *   mixdown: L = 3.15625 × s, R = 1.28125 × s (peak 0.79 / 0.32 —
   *            comfortably inside the clamp). */
  OeClipAudio clip_a = { .gain = 1024, .pan = 256 };

  g_assert_true (oe_project_set_clip_audio (project, track_a, 0, &clip_a, NULL));

  OeClipAudio clip_b = { .gain = 512, .pan = 768 };

  g_assert_true (oe_project_set_clip_audio (project, track_b, 0, &clip_b, NULL));

  OeTrackAudio ta = { .volume = 2048, .pan = 512, .mute = 0, .solo = 0 };
  OeTrackAudio tb = { .volume = 512, .pan = 384, .mute = 0, .solo = 0 };

  g_assert_true (oe_project_set_track_audio (project, track_a, &ta, NULL));
  g_assert_true (oe_project_set_track_audio (project, track_b, &tb, NULL));

  /* ---- Preview side: play the sequence through the real mixer. ---- */

  OePlaybackSession *session = oe_playback_session_new ((const OeProject *) project);

  ParityClock clock = { G_GINT64_CONSTANT (1000000) };

  oe_playback_session_set_time_source (session, parity_fake_time, &clock);

  MixCapture cap = { 0 };

  oe_playback_session_set_mix_func (session, collect_mix, &cap);

  GError *error = NULL;

  g_assert_true (oe_playback_session_play (session, &error));
  g_assert_no_error (error);

  /* Drive the transport across the sequence; pump the main context so
   * the worker's decoded deliveries (g_main_context_invoke) reach the
   * mixer. The session feeds position + 250 ms lookahead, so the first
   * mixed window starts near 250 ms and the capture runs to the clip
   * end at 600 ms. Bounded loop + progress guard: a stalled capture
   * fails with the window actually captured. */
  const gint64 target_us = 550000;
  gint64 last_frames = 0;
  int stall = 0;

  for (int i = 0; i < 20000; i++)
    {
      const gint64 have_us = cap.frames * G_GINT64_CONSTANT (1000000) / MAX (cap.rate, 1);

      if (have_us >= target_us || oe_playback_session_get_state (session) != OE_PLAYBACK_PLAYING)
        break;

      clock.now_us += 40000;
      oe_playback_session_tick (session);

      while (g_main_context_iteration (NULL, FALSE))
        ;

      g_usleep (2000); /* the worker decodes off-thread; yield to it */

      if (cap.frames == last_frames)
        stall++;
      else
        stall = 0;
      last_frames = cap.frames;

      g_assert_cmpint (stall, <, 3000); /* ~6 s of no progress: broken */
    }

  const gint64 captured_us = cap.frames * G_GINT64_CONSTANT (1000000) / MAX (cap.rate, 1);

  g_assert_cmpint ((int) captured_us, >=, 250000);

  oe_playback_session_stop (session);
  g_clear_pointer (&session, oe_playback_session_free); /* drains the worker */

  /* ---- Export side: the mixdown of the same project. ---- */

  gchar *dest = g_build_filename (fx->dir, "out.mp4", NULL);

  g_assert_true (
      run_export (project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL, NULL, NULL, NULL, &error));
  g_assert_no_error (error);

  FloatAudio mix;

  decode_audio_float (dest, &mix);

  /* ---- Per-channel parity over the steady mid window. ---- */
  for (int c = 0; c < 2; c++)
    {
      const gdouble preview = capture_mean_abs (&cap, 300000, 500000, c);
      const gdouble exported = channel_mean_abs (&mix, 300000, 500000, c);

      g_assert_cmpfloat (preview, >, 0.02); /* real signal on this channel */
      g_assert_cmpfloat (fabs (preview - exported), <=, 0.02);
    }

  /* The distinct pans must be visible on BOTH sides (sanity on the
   * fixture itself: left-leaning A + right-leaning B pair stages). */
  g_assert_cmpfloat (capture_mean_abs (&cap, 300000, 500000, 0), >,
                     1.5 * capture_mean_abs (&cap, 300000, 500000, 1));
  g_assert_cmpfloat (channel_mean_abs (&mix, 300000, 500000, 0), >,
                     1.5 * channel_mean_abs (&mix, 300000, 500000, 1));

  mix_capture_clear (&cap);
  float_audio_clear (&mix);
  g_free (dest);
  g_object_unref (project);
  g_free (red);
  g_free (blue);
  g_free (wav);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  /* /export/parity-audio-mix-two-tracks plays through the real
   * session, which opens an SDL stream at play(): init the audio
   * adapter for the whole run (dummy driver via the test
   * environment). Failure only disables the parity test. */
  GError *audio_error = NULL;

  audio_ready = oe_audio_output_init (&audio_error);

  if (!audio_ready)
    {
      g_printerr ("audio init failed: %s\n", audio_error->message);
      g_error_free (audio_error);
    }

#define OE_ADD(path, fn)                                                                           \
  g_test_add ((path), OeFixtures, NULL, fixture_set_up, (fn), fixture_tear_down)

  OE_ADD ("/export/frame-grid", test_frame_grid);
  OE_ADD ("/export/parity-straight-cut", test_parity_straight_cut);
  g_test_add_func ("/export/blend-unit", test_blend_unit);
  OE_ADD ("/export/two-layer-seam", test_two_layer_seam);
  OE_ADD ("/export/parity-two-layer", test_parity_two_layer);
  OE_ADD ("/export/transition-blend", test_transition_blend);
  OE_ADD ("/export/mixdown-pan-positions", test_mixdown_pan_positions);
  OE_ADD ("/export/mixdown-gain-ratio", test_mixdown_gain_ratio);
  OE_ADD ("/export/mixdown-mute-solo-matrix", test_mixdown_mute_solo_matrix);
  OE_ADD ("/export/mixdown-fade-ratio", test_mixdown_fade_ratio);
  OE_ADD ("/export/parity-transition-fade", test_parity_transition_fade);
  OE_ADD ("/export/parity-audio-mix-two-tracks", test_parity_audio_mix_two_tracks);
  OE_ADD ("/export/container-truth", test_container_truth);
  OE_ADD ("/export/content-round-trip", test_content_round_trip);
  OE_ADD ("/export/mixdown-sums", test_mixdown_sums);
  OE_ADD ("/export/cancellation", test_cancellation);
  OE_ADD ("/export/atomic-failure", test_atomic_failure);
  OE_ADD ("/export/atomic-failure-unwritable-dir", test_atomic_failure_unwritable_dir);

#undef OE_ADD

  const int result = g_test_run ();

  if (audio_ready)
    oe_audio_output_shutdown ();

  return result;
}
