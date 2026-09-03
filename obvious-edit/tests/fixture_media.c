/* fixture_media.c — runtime media fixture generator implementation.
 *
 * Three libavformat/libavcodec encoders do all the work; nothing here is
 * specific to the code under test. A generation failure is a test-authoring
 * bug, so the helpers abort through g_assert_* rather than error paths.
 */

#include "fixture_media.h"

#include <math.h>
#include <string.h>

#include <glib/gstdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

/* ------------------------------------------------------------------ */
/* Small shared helpers                                                */
/* ------------------------------------------------------------------ */

static void
log_ffmpeg_error (int rv, const char *what)
{
  gchar buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };

  av_strerror (rv, buf, sizeof (buf));
  g_error ("%s failed: %s", what, buf);
}

static void
expect (int rv, const char *what)
{
  if (rv < 0)
    log_ffmpeg_error (rv, what);
}

static void
write_all (AVFormatContext *fmt, AVPacket *pkt, AVCodecContext *enc)
{
  int rv = avcodec_receive_packet (enc, pkt);

  while (rv == 0)
    {
      av_packet_rescale_ts (pkt, enc->time_base, fmt->streams[0]->time_base);
      pkt->stream_index = 0;
      expect (av_interleaved_write_frame (fmt, pkt), "av_interleaved_write_frame");
      av_packet_unref (pkt);
      rv = avcodec_receive_packet (enc, pkt);
    }

  /* EAGAIN mid-encode, EOF once the encoder has been flushed. */
  g_assert (rv == AVERROR (EAGAIN) || rv == AVERROR_EOF);
}

/* ------------------------------------------------------------------ */
/* WAV: pcm_s16le mono sine                                            */
/* ------------------------------------------------------------------ */

static void
write_wav (const gchar *path)
{
  const AVCodec *enc = avcodec_find_encoder (AV_CODEC_ID_PCM_S16LE);

  g_assert_nonnull (enc);

  AVFormatContext *fmt = NULL;

  g_assert (avformat_alloc_output_context2 (&fmt, NULL, NULL, path) == 0);
  g_assert_nonnull (fmt);

  AVCodecContext *ctx = avcodec_alloc_context3 (enc);

  g_assert_nonnull (ctx);
  ctx->sample_rate = OE_FIXTURE_WAV_RATE;
  av_channel_layout_default (&ctx->ch_layout, OE_FIXTURE_WAV_CHANNELS);
  ctx->sample_fmt = AV_SAMPLE_FMT_S16;
  ctx->time_base = (AVRational) { 1, OE_FIXTURE_WAV_RATE };
  g_assert (avcodec_open2 (ctx, enc, NULL) == 0);

  AVStream *st = avformat_new_stream (fmt, NULL);

  g_assert_nonnull (st);
  g_assert (avcodec_parameters_from_context (st->codecpar, ctx) == 0);
  st->time_base = ctx->time_base;

  if (!(fmt->oformat->flags & AVFMT_NOFILE))
    g_assert (avio_open (&fmt->pb, path, AVIO_FLAG_WRITE) == 0);
  g_assert (avformat_write_header (fmt, NULL) == 0);

  const gint64 total = (gint64) OE_FIXTURE_WAV_RATE * OE_FIXTURE_WAV_DURATION_US / G_USEC_PER_SEC;
  AVFrame *frame = av_frame_alloc ();
  AVPacket *pkt = av_packet_alloc ();

  g_assert_nonnull (frame);
  g_assert_nonnull (pkt);
  frame->format = ctx->sample_fmt;
  frame->nb_samples = 1024;
  av_channel_layout_copy (&frame->ch_layout, &ctx->ch_layout);
  g_assert (av_frame_get_buffer (frame, 0) == 0);

  gint64 written = 0;
  gint64 pts = 0;

  while (written < total)
    {
      g_assert (av_frame_make_writable (frame) == 0);
      gint16 *samples = (gint16 *) frame->data[0];
      int n = (int) MIN ((gint64) frame->nb_samples, total - written);

      for (int i = 0; i < n; i++)
        {
          double t = (double) (written + i) / OE_FIXTURE_WAV_RATE;
          /* 440 Hz sine at 0.8 full scale — well inside int16 range. */
          samples[i] = (gint16) (sinf ((float) (2.0 * M_PI * 440.0 * t)) * 26000.0f);
        }

      frame->nb_samples = n;
      frame->pts = pts;
      g_assert (avcodec_send_frame (ctx, frame) == 0);
      write_all (fmt, pkt, ctx);
      pts += n;
      written += n;
    }

  g_assert (avcodec_send_frame (ctx, NULL) == 0);
  write_all (fmt, pkt, ctx);
  g_assert (av_write_trailer (fmt) == 0);

  av_packet_free (&pkt);
  av_frame_free (&frame);
  if (!(fmt->oformat->flags & AVFMT_NOFILE))
    avio_closep (&fmt->pb);
  avformat_free_context (fmt);
  avcodec_free_context (&ctx);
}

/* ------------------------------------------------------------------ */
/* AVI: MJPEG video, no audio                                          */
/* ------------------------------------------------------------------ */

static void
fill_video_frame (AVFrame *frame, int index)
{
  /* Flat synthetic content with per-frame variation so the thumbnail
   * decodes real pixels; Y plane intensity ramps, U/V carry a pattern. */
  const int shift = (index * 16) % 256;

  for (int y = 0; y < frame->height; y++)
    {
      guint8 *row = frame->data[0] + (gsize) y * frame->linesize[0];

      for (int x = 0; x < frame->width; x++)
        row[x] = (guint8) (((x * 3 + y * 2 + shift) % 224) + 16);
    }

  for (int y = 0; y < frame->height / 2; y++)
    {
      guint8 *u = frame->data[1] + (gsize) y * frame->linesize[1];
      guint8 *v = frame->data[2] + (gsize) y * frame->linesize[2];

      for (int x = 0; x < frame->width / 2; x++)
        {
          u[x] = (guint8) (128 + ((x + index) % 32) - 16);
          v[x] = (guint8) (128 - ((y + index) % 32) + 16);
        }
    }
}

static void
write_avi (const gchar *path)
{
  const AVCodec *enc = avcodec_find_encoder (AV_CODEC_ID_MJPEG);

  g_assert_nonnull (enc);

  AVFormatContext *fmt = NULL;

  g_assert (avformat_alloc_output_context2 (&fmt, NULL, "avi", path) == 0);
  g_assert_nonnull (fmt);

  AVCodecContext *ctx = avcodec_alloc_context3 (enc);

  g_assert_nonnull (ctx);
  ctx->width = OE_FIXTURE_AVI_WIDTH;
  ctx->height = OE_FIXTURE_AVI_HEIGHT;
  ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
  ctx->time_base = (AVRational) { OE_FIXTURE_AVI_FPS_DEN, OE_FIXTURE_AVI_FPS_NUM };
  ctx->framerate = (AVRational) { OE_FIXTURE_AVI_FPS_NUM, OE_FIXTURE_AVI_FPS_DEN };
  if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  g_assert (avcodec_open2 (ctx, enc, NULL) == 0);

  AVStream *st = avformat_new_stream (fmt, NULL);

  g_assert_nonnull (st);
  g_assert (avcodec_parameters_from_context (st->codecpar, ctx) == 0);
  st->time_base = ctx->time_base;
  st->avg_frame_rate = ctx->framerate;
  st->nb_frames = OE_FIXTURE_AVI_FRAMES;

  if (!(fmt->oformat->flags & AVFMT_NOFILE))
    g_assert (avio_open (&fmt->pb, path, AVIO_FLAG_WRITE) == 0);
  g_assert (avformat_write_header (fmt, NULL) == 0);

  AVFrame *frame = av_frame_alloc ();
  AVPacket *pkt = av_packet_alloc ();

  g_assert_nonnull (frame);
  g_assert_nonnull (pkt);
  frame->format = ctx->pix_fmt;
  frame->width = ctx->width;
  frame->height = ctx->height;
  g_assert (av_frame_get_buffer (frame, 0) == 0);

  for (int i = 0; i < OE_FIXTURE_AVI_FRAMES; i++)
    {
      g_assert (av_frame_make_writable (frame) == 0);
      fill_video_frame (frame, i);
      frame->pts = (gint64) i * OE_FIXTURE_AVI_FPS_DEN;
      g_assert (avcodec_send_frame (ctx, frame) == 0);
      write_all (fmt, pkt, ctx);
    }

  g_assert (avcodec_send_frame (ctx, NULL) == 0);
  write_all (fmt, pkt, ctx);
  g_assert (av_write_trailer (fmt) == 0);

  av_packet_free (&pkt);
  av_frame_free (&frame);
  if (!(fmt->oformat->flags & AVFMT_NOFILE))
    avio_closep (&fmt->pb);
  avformat_free_context (fmt);
  avcodec_free_context (&ctx);
}

/* ------------------------------------------------------------------ */
/* PNG: single encoded frame written straight to disk                  */
/* ------------------------------------------------------------------ */

static void
write_png (const gchar *path)
{
  const AVCodec *enc = avcodec_find_encoder (AV_CODEC_ID_PNG);

  g_assert_nonnull (enc);

  AVCodecContext *ctx = avcodec_alloc_context3 (enc);

  g_assert_nonnull (ctx);
  ctx->width = OE_FIXTURE_PNG_WIDTH;
  ctx->height = OE_FIXTURE_PNG_HEIGHT;
  ctx->pix_fmt = AV_PIX_FMT_RGB24;
  ctx->time_base = (AVRational) { 1, 25 };
  g_assert (avcodec_open2 (ctx, enc, NULL) == 0);

  AVFrame *frame = av_frame_alloc ();
  AVPacket *pkt = av_packet_alloc ();

  g_assert_nonnull (frame);
  g_assert_nonnull (pkt);
  frame->format = ctx->pix_fmt;
  frame->width = ctx->width;
  frame->height = ctx->height;
  g_assert (av_frame_get_buffer (frame, 0) == 0);
  g_assert (av_frame_make_writable (frame) == 0);

  for (int y = 0; y < frame->height; y++)
    {
      guint8 *row = frame->data[0] + (gsize) y * frame->linesize[0];

      for (int x = 0; x < frame->width; x++)
        {
          row[(gsize) x * 3 + 0] = (guint8) (x * 255 / frame->width);
          row[(gsize) x * 3 + 1] = (guint8) (y * 255 / frame->height);
          row[(gsize) x * 3 + 2] = 128;
        }
    }

  frame->pts = 0;
  g_assert (avcodec_send_frame (ctx, frame) == 0);
  g_assert (avcodec_receive_packet (ctx, pkt) == 0);

  /* A PNG packet is a complete, self-contained PNG file: writing its bytes
   * directly gives a valid still image without an image2 muxer pattern. */
  GError *error = NULL;

  g_file_set_contents (path, (const gchar *) pkt->data, pkt->size, &error);
  g_assert_no_error (error);

  av_packet_free (&pkt);
  av_frame_free (&frame);
  avcodec_free_context (&ctx);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static void
write_text (const gchar *path, const gchar *content)
{
  GError *error = NULL;

  g_file_set_contents (path, content, -1, &error);
  g_assert_no_error (error);
}

gboolean
oe_fixtures_create (OeFixtures *fx, GError **error)
{
  g_return_val_if_fail (fx != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  memset (fx, 0, sizeof (*fx));

  fx->dir = g_dir_make_tmp ("oe-fixtures-XXXXXX", error);
  if (fx->dir == NULL)
    return FALSE;

  fx->wav_path = g_build_filename (fx->dir, "tone.wav", NULL);
  fx->avi_path = g_build_filename (fx->dir, "clip.avi", NULL);
  fx->png_path = g_build_filename (fx->dir, "still.png", NULL);
  fx->txt_path = g_build_filename (fx->dir, "notes.txt", NULL);
  fx->empty_path = g_build_filename (fx->dir, "empty.bin", NULL);

  write_wav (fx->wav_path);
  write_avi (fx->avi_path);
  write_png (fx->png_path);
  write_text (fx->txt_path, "This is not media. It is a text file with a few lines that\n"
                            "no container probe should ever accept.\n");
  write_text (fx->empty_path, "");

  return TRUE;
}

static void
remove_dir_recursive (const gchar *dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);

  if (d == NULL)
    return;

  const gchar *name;

  while ((name = g_dir_read_name (d)) != NULL)
    {
      gchar *child = g_build_filename (dir, name, NULL);

      if (g_file_test (child, G_FILE_TEST_IS_DIR))
        remove_dir_recursive (child);
      else
        g_remove (child);
      g_free (child);
    }

  g_dir_close (d);
  g_rmdir (dir);
}

void
oe_fixtures_free (OeFixtures *fx)
{
  if (fx->dir != NULL)
    remove_dir_recursive (fx->dir);

  g_clear_pointer (&fx->dir, g_free);
  g_clear_pointer (&fx->wav_path, g_free);
  g_clear_pointer (&fx->avi_path, g_free);
  g_clear_pointer (&fx->png_path, g_free);
  g_clear_pointer (&fx->txt_path, g_free);
  g_clear_pointer (&fx->empty_path, g_free);
}
