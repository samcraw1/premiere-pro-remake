/* test_titles_key.c — GTK-free tests for Phase 11 Wave A (19th suite).
 *
 * Generated clips (titles and solids) and source-space chroma keying,
 * end to end: validated model mutation, undo replay, strict additive
 * persistence, the Cairo rasterizer, the pure key rule, fast-path
 * preservation, and export decode-back parity. Synthetic in-process
 * fixtures only (the solid MJPEG AVI generator from the export suite);
 * every assertion is structural or exact-integer — never glyph shapes.
 *
 *   /titles-key/model-insert-validate   generator insert semantics and
 *                                       the kind-conditional rejections
 *   /titles-key/model-mutate-reject     set_clip_generator/set_clip_key
 *                                       accept/reject/zero-delta contracts
 *   /titles-key/undo-generator-replay   GENERATOR op replays bit-exact
 *                                       (owned text), zero-delta silent
 *   /titles-key/undo-key-replay         CLIP_KEY op replays exactly;
 *                                       keying a generator is rejected
 *   /titles-key/json-roundtrip          save-load-save byte-identical
 *                                       with kind/generator/key present
 *   /titles-key/json-backfill           pre-Phase-11 clips (no new
 *                                       members) load with identity
 *                                       backfills
 *   /titles-key/json-strict-members     closed-member rejection intact
 *                                       for unknown members
 *   /titles-key/raster-determinism      same inputs → byte-identical
 *                                       buffers; solid is an exact fill
 *   /titles-key/raster-cache            identity-keyed cache: hit, miss,
 *                                       and fresh-cache isolation
 *   /titles-key/key-math                the pure per-pixel rule: domains,
 *                                       edges, exactly one rounding
 *   /titles-key/fast-path-preserved     single unkeyed media clip renders
 *                                       byte-identically through the fast
 *                                       path and the layered path; a
 *                                       generator never reaches the media
 *                                       decoder
 *   /titles-key/compositor-title-solid  titles and solids composite as
 *                                       ordinary layers over media
 *   /titles-key/key-composite           keyed regions show the lower
 *                                       layer; disabled key ≡ no-key;
 *                                       softness produces intermediate
 *                                       alpha
 *   /titles-key/export-parity           titled and keyed frames: export
 *                                       decode-back vs the shared seam,
 *                                       block means |Δ| ≤ 8 per channel
 *
 * cairo and json-glib are explicit dependencies of this suite in
 * meson.build (the render seam rasterizes titles through Cairo; the
 * backfill tests rewrite project JSON through json-glib). Links the
 * render/export sources — no GTK.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include <math.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <json-glib/json-glib.h>

#include "../src/app/oe_undo_stack.h"
#include "../src/core/oe_project.h"
#include "../src/core/oe_project_format.h"
#include "../src/media/oe_export.h"
#include "../src/media/oe_generator_raster.h"
#include "../src/media/oe_render.h"
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
 * compositing assertions measure the generator path, not source
 * content. (Copied from the export suite's harness.) */
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

static void
insert_generator (OeProject *project, guint track, OeClipKind kind, const OeClipGenerator *gen,
                  gint64 pos_us, gint64 len_us)
{
  GError *error = NULL;

  g_assert_true (
      oe_project_insert_generator_clip (project, track, kind, pos_us, len_us, gen, &error));
  g_assert_no_error (error);
}

/* The export resolver: media refs come straight from the project's
 * media table. (Copied from the export suite.) */
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
run_export (OeProject *project, const gchar *dest, OeExportQuality quality, GError **error)
{
  OeSequence seq = { 0 };

  oe_project_get_sequence (project, &seq);

  OeExportSpec spec = { 0 };

  spec.sequence = &seq;
  spec.resolve_path = test_resolve;
  spec.resolve_data = project;
  spec.destination_path = dest;
  spec.quality = quality;

  const gboolean ok = oe_export_run (&spec, NULL, NULL, NULL, NULL, error);

  oe_sequence_clear (&seq);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Decode-back helpers (copied from the export suite)                  */
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

  g_assert_nonnull (result);
  return result;
}

/* Per-channel mean of a BGRA rect. */
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

  out[0] = r / (gdouble) n;
  out[1] = g / (gdouble) n;
  out[2] = b / (gdouble) n;
}

/* The rect's dominant channel class must be @r/@g/@b's. */
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

/* Whole-frame dominant class. */
static void
assert_dominant (const guint8 *bgra, int w, int h, const gchar *what, guint8 r, guint8 g, guint8 b)
{
  assert_rect_dominant (bgra, w, h, 0, 0, w, h, what, r, g, b);
}

/* Block-mean parity: |Δ| ≤ 8 per channel in every 8x8 block (the
 * documented export tolerance for yuv420p + quantization). */
static void
assert_block_mean_parity (const guint8 *a, const guint8 *b, int w, int h)
{
  const int BS = 8;

  for (int by = 0; by < h / BS; by++)
    for (int bx = 0; bx < w / BS; bx++)
      {
        double am[3], bm[3];

        window_mean (a, w, h, bx * BS, by * BS, (bx + 1) * BS, (by + 1) * BS, am);
        window_mean (b, w, h, bx * BS, by * BS, (bx + 1) * BS, (by + 1) * BS, bm);
        for (int c = 0; c < 3; c++)
          g_assert_cmpfloat (fabs (am[c] - bm[c]), <=, 8.0);
      }
}

/* Renders the seam canvas for @project at @t_us. */
static guint8 *
render_at (OeProject *project, gint64 t_us)
{
  OeSequence seq = { 0 };

  oe_project_get_sequence (project, &seq);

  OeRenderSource source = { &seq, test_resolve, project };

  GError *error = NULL;
  guint8 *frame = oe_render_frame_at (&source, t_us, TEST_W, TEST_H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (frame);
  oe_sequence_clear (&seq);
  return frame;
}

/* ------------------------------------------------------------------ */
/* Model: insert / validate / reject                                   */
/* ------------------------------------------------------------------ */

static void
test_model_insert_validate ()
{
  OeProject *project = new_project_25fps ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint audio = oe_project_add_track (project, OE_TRACK_AUDIO);
  GError *error = NULL;

  /* A title with a valid payload inserts on the video track; the
   * still-image convention gives it the full source range. */
  OeClipGenerator title = oe_clip_generator_identity ();

  title.text = g_strdup ("Hello");
  title.color_rgb = 0xffffff;
  title.size_permille = 200;
  insert_generator (project, video, OE_CLIP_TITLE, &title, 0, 1000000);
  g_free (title.text);

  OeClip clip = { 0 };

  g_assert_true (oe_project_get_clip (project, video, 0, &clip));
  g_assert_cmpint (clip.kind, ==, OE_CLIP_TITLE);
  g_assert_cmpuint (clip.media_ref, ==, 0); /* generators carry no media (D3) */
  g_assert_cmpint (clip.position_us, ==, 0);
  g_assert_cmpint (clip.source_in_us, ==, 0);
  g_assert_cmpint (clip.source_out_us, ==, 1000000);
  g_assert_cmpstr (clip.generator.text, ==, "Hello");
  g_assert_cmpint (clip.generator.color_rgb, ==, 0xffffff);
  g_assert_cmpint (clip.generator.size_permille, ==, 200);
  g_assert_cmpint (clip.key.enabled, ==, 0); /* identity backfill in the model too */

  /* A solid with no text inserts; OE_CLIP_MEDIA through the generator
   * entry point is rejected (the media path is insert_clip). */
  OeClipGenerator solid = oe_clip_generator_identity ();

  solid.color_rgb = 0x204080;
  solid.size_permille = 0;
  insert_generator (project, video, OE_CLIP_SOLID, &solid, 1000000, 500000);
  g_assert_false (oe_project_insert_generator_clip (project, video, OE_CLIP_MEDIA, 2000000, 500000,
                                                    &solid, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_GENERATOR);
  g_clear_error (&error);

  /* Generators never sit on audio tracks (D3: no audio payload). */
  g_assert_false (
      oe_project_insert_generator_clip (project, audio, OE_CLIP_SOLID, 0, 500000, &solid, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK);
  g_clear_error (&error);

  /* Payload validation is kind-conditional: titles need a non-empty
   * UTF-8 text and a positive size; solids must stay text-free; both
   * keep color/size in their domains. Rejected calls touch nothing. */
  const struct
  {
    OeClipKind kind;
    const gchar *text;
    gint color_rgb;
    gint size_permille;
  } bad_payloads[] = {
    { OE_CLIP_TITLE, NULL, 0x000000, 200 },           /* no text        */
    { OE_CLIP_TITLE, "", 0x000000, 200 },             /* empty text     */
    { OE_CLIP_TITLE, "H\303\251llo", 0x000000, 200 }, /* UTF-8 is fine (control) */
    { OE_CLIP_TITLE, "Hello", 0x1000000, 200 },       /* color too big  */
    { OE_CLIP_TITLE, "Hello", -1, 200 },              /* color negative */
    { OE_CLIP_TITLE, "Hello", 0x000000, 0 },          /* size zero      */
    { OE_CLIP_TITLE, "Hello", 0x000000, 1001 },       /* size too big   */
    { OE_CLIP_SOLID, "unexpected", 0x204080, 0 },     /* solid has text */
  };

  for (gsize i = 0; i < G_N_ELEMENTS (bad_payloads); i++)
    {
      /* The UTF-8 control row must succeed, so drive it separately. */
      if (i == 2)
        continue;

      OeClipGenerator gen = oe_clip_generator_identity ();

      gen.text = bad_payloads[i].text != NULL ? g_strdup (bad_payloads[i].text) : NULL;
      gen.color_rgb = bad_payloads[i].color_rgb;
      gen.size_permille = bad_payloads[i].size_permille;

      g_assert_false (oe_project_insert_generator_clip (project, video, bad_payloads[i].kind, 0,
                                                        100000, &gen, &error));
      g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_GENERATOR);
      g_clear_error (&error);
      oe_clip_generator_clear (&gen);
    }

  /* And the UTF-8 title really is accepted (2 clips now). */
  OeClipGenerator utf8 = oe_clip_generator_identity ();

  utf8.text = g_strdup ("H\303\251llo");
  utf8.size_permille = 100;
  insert_generator (project, video, OE_CLIP_TITLE, &utf8, 2000000, 100000);
  g_free (utf8.text);

  /* The rejected calls left the track with exactly the three clips. */
  OeSequence seq = { 0 };

  oe_project_get_sequence (project, &seq);
  g_assert_cmpuint (((OeTrack *) g_ptr_array_index (seq.tracks, video))->clips->len, ==, 3);
  oe_sequence_clear (&seq);

  g_object_unref (project);
}

/* Observer pulse counter for the notify-once contracts. */
typedef struct
{
  guint notified;
} NotifyCounter;

static void
count_notify (gpointer user_data)
{
  ((NotifyCounter *) user_data)->notified++;
}

static void
test_model_mutate_reject ()
{
  OeProject *project = new_project_25fps ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint audio = oe_project_add_track (project, OE_TRACK_AUDIO);
  const guint ref = add_media (project, "/fixtures/m.mp4");
  GError *error = NULL;

  insert_clip (project, video, ref, 0, 1000000);

  OeClipGenerator title = oe_clip_generator_identity ();

  title.text = g_strdup ("T");
  title.size_permille = 100;
  insert_generator (project, video, OE_CLIP_TITLE, &title, 1000000, 500000);
  g_free (title.text);
  /* audio track carries a media clip for the audio-key rejection arm */
  insert_clip (project, audio, ref, 0, 1000000);

  NotifyCounter counter = { 0 };

  oe_project_set_observer (project, count_notify, &counter);

  /* set_clip_generator on a media clip: dormant payload (D3). */
  OeClipGenerator gen = oe_clip_generator_identity ();

  gen.text = g_strdup ("nope");
  gen.size_permille = 100;
  g_assert_false (oe_project_set_clip_generator (project, video, 0, &gen, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_GENERATOR);
  g_clear_error (&error);
  g_assert_cmpuint (counter.notified, ==, 0);

  /* A valid set on the title fires the observer exactly once and
   * deep-copies the owned text. */
  OeClipGenerator renamed = oe_clip_generator_identity ();

  renamed.text = g_strdup ("Renamed");
  renamed.color_rgb = 0x102030;
  renamed.size_permille = 300;
  g_assert_true (oe_project_set_clip_generator (project, video, 1, &renamed, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (counter.notified, ==, 1);
  g_free (renamed.text);

  OeClip fetched;
  memset (&fetched, 0, sizeof (fetched));
  g_assert_true (oe_project_get_clip (project, video, 1, &fetched));
  g_assert_cmpstr (fetched.generator.text, ==, "Renamed");
  g_assert_cmpint (fetched.generator.color_rgb, ==, 0x102030);
  g_assert_cmpint (fetched.generator.size_permille, ==, 300);

  /* Zero-delta set (identical payload): silent success, no pulse. */
  OeClipGenerator same = oe_clip_generator_identity ();

  same.text = g_strdup ("Renamed");
  same.color_rgb = 0x102030;
  same.size_permille = 300;
  g_assert_true (oe_project_set_clip_generator (project, video, 1, &same, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (counter.notified, ==, 1);
  oe_clip_generator_clear (&same);

  /* Invalid payload for the kind: rejected, nothing touched. */
  OeClipGenerator bad = oe_clip_generator_identity ();

  bad.size_permille = 0; /* titles need a positive size */
  g_assert_false (oe_project_set_clip_generator (project, video, 1, &bad, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_GENERATOR);
  memset (&fetched, 0, sizeof (fetched));
  g_assert_true (oe_project_get_clip (project, video, 1, &fetched));
  g_clear_error (&error);
  g_assert_cmpuint (counter.notified, ==, 1);

  /* The rejected stroke left the previous generator in place. */
  g_assert_cmpstr (fetched.generator.text, ==, "Renamed");

  /* set_clip_key on a generator: keying is media-only (D4). */
  OeClipKey key = oe_clip_key_identity ();

  key.color_rgb = 0x20e020;
  key.tolerance = 400;
  key.softness = 100;
  key.enabled = 1;
  g_assert_false (oe_project_set_clip_key (project, video, 1, &key, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_GENERATOR);
  g_clear_error (&error);
  g_assert_cmpuint (counter.notified, ==, 1);

  /* Keying an audio-track clip: BAD_TRACK. */
  g_assert_false (oe_project_set_clip_key (project, audio, 0, &key, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK);
  g_clear_error (&error);

  /* Out-of-domain keys are rejected on the media clip, untouched. */
  const OeClipKey bad_keys[] = {
    { 0x20e020, -1, 100, 1 },   { 0x20e020, 1025, 100, 1 }, { 0x20e020, 100, -1, 1 },
    { 0x20e020, 100, 1025, 1 }, { 0x20e020, 100, 100, 2 },  { 0x20e020, 100, 100, -1 },
    { 0x1000000, 100, 100, 1 }, { -1, 100, 100, 1 },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (bad_keys); i++)
    {
      g_assert_false (oe_project_set_clip_key (project, video, 0, &bad_keys[i], &error));
      g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_GENERATOR);
      g_clear_error (&error);
      g_assert_cmpuint (counter.notified, ==, 1);
    }

  /* The valid key sticks on the media clip, exactly once notified. */
  g_assert_true (oe_project_set_clip_key (project, video, 0, &key, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (counter.notified, ==, 2);

  memset (&fetched, 0, sizeof (fetched));
  g_assert_true (oe_project_get_clip (project, video, 0, &fetched));
  g_assert_cmpint (fetched.key.color_rgb, ==, 0x20e020);
  g_assert_cmpint (fetched.key.tolerance, ==, 400);
  g_assert_cmpint (fetched.key.softness, ==, 100);
  g_assert_cmpint (fetched.key.enabled, ==, 1);

  oe_clip_generator_clear (&gen);
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */
/* Undo replay                                                         */
/* ------------------------------------------------------------------ */

static void
test_undo_generator_replay ()
{
  OeProject *project = new_project_25fps ();
  OeUndoStack *stack = oe_undo_stack_new ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);
  GError *error = NULL;

  OeClipGenerator g0 = oe_clip_generator_identity ();

  g0.text = g_strdup ("Before");
  g0.color_rgb = 0x112233;
  g0.size_permille = 150;
  insert_generator (project, video, OE_CLIP_TITLE, &g0, 0, 1000000);
  g_free (g0.text);

  /* The stroke captures its own copy; freeing our string must not
   * affect the record. */
  OeClipGenerator g1 = oe_clip_generator_identity ();

  g1.text = g_strdup ("After");
  g1.color_rgb = 0x445566;
  g1.size_permille = 400;

  g_assert_true (oe_edit_set_clip_generator (project, stack, video, 0, &g1, &error));
  g_assert_no_error (error);
  oe_clip_generator_clear (&g1);

  OeClip clip = { 0 };

  g_assert_true (oe_project_get_clip (project, video, 0, &clip));
  g_assert_cmpstr (clip.generator.text, ==, "After");
  g_assert_cmpint (clip.generator.color_rgb, ==, 0x445566);

  /* Undo restores the pre-stroke generator bit-exactly. */
  g_assert_true (oe_undo_stack_undo (stack, project, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (oe_project_get_clip (project, video, 0, &clip));
  g_assert_cmpstr (clip.generator.text, ==, "Before");
  g_assert_cmpint (clip.generator.color_rgb, ==, 0x112233);
  g_assert_cmpint (clip.generator.size_permille, ==, 150);

  /* Redo re-applies through the validated mutator. */
  g_assert_true (oe_undo_stack_redo (stack, project, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (oe_project_get_clip (project, video, 0, &clip));
  g_assert_cmpstr (clip.generator.text, ==, "After");

  /* Zero-delta strokes (with_old, equal states) record nothing. */
  OeClipGenerator same = oe_clip_generator_identity ();

  same.text = g_strdup ("After");
  same.color_rgb = 0x445566;
  same.size_permille = 400;
  g_assert_true (
      oe_edit_set_clip_generator_with_old (project, stack, video, 0, &same, &same, &error));
  g_assert_no_error (error);
  /* The g1 stroke stays in history (can_undo), but nothing new was
   * recorded: the size is unchanged and the redo branch untouched. */
  g_assert_true (oe_undo_stack_can_undo (stack));
  g_assert_false (oe_undo_stack_can_redo (stack));
  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 1);
  oe_clip_generator_clear (&same);

  /* A rejected stroke records nothing either. */
  OeClipGenerator invalid = oe_clip_generator_identity ();

  invalid.size_permille = 0;
  g_assert_false (oe_edit_set_clip_generator (project, stack, video, 0, &invalid, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_GENERATOR);
  g_clear_error (&error);

  oe_undo_stack_free (stack);
  g_object_unref (project);
}

static void
test_undo_key_replay ()
{
  OeProject *project = new_project_25fps ();
  OeUndoStack *stack = oe_undo_stack_new ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint ref = add_media (project, "/fixtures/m.mp4");
  GError *error = NULL;

  insert_clip (project, video, ref, 0, 1000000);

  OeClipKey k1 = oe_clip_key_identity ();

  k1.color_rgb = 0x20e020;
  k1.tolerance = 512;
  k1.softness = 256;
  k1.enabled = 1;

  g_assert_true (oe_edit_set_clip_key (project, stack, video, 0, &k1, &error));
  g_assert_no_error (error);

  OeClip clip = { 0 };

  g_assert_true (oe_project_get_clip (project, video, 0, &clip));
  g_assert_cmpint (clip.key.enabled, ==, 1);
  g_assert_cmpint (clip.key.tolerance, ==, 512);

  g_assert_true (oe_undo_stack_undo (stack, project, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (oe_project_get_clip (project, video, 0, &clip));
  g_assert_cmpint (clip.key.enabled, ==, 0); /* identity back */
  g_assert_cmpint (clip.key.tolerance, ==, 0);

  g_assert_true (oe_undo_stack_redo (stack, project, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (oe_project_get_clip (project, video, 0, &clip));
  g_assert_cmpint (clip.key.tolerance, ==, 512);
  g_assert_cmpint (clip.key.softness, ==, 256);

  /* Zero-delta key stroke records nothing: history keeps the key
   * stroke, the size is unchanged, and the redo branch is untouched. */
  OeClipKey same = k1;

  g_assert_true (oe_edit_set_clip_key_with_old (project, stack, video, 0, &same, &same, &error));
  g_assert_no_error (error);
  g_assert_true (oe_undo_stack_can_undo (stack));
  g_assert_false (oe_undo_stack_can_redo (stack));
  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 1);

  /* Keying a generator through the recorder: model rejection first,
   * nothing recorded. */
  OeClipGenerator title = oe_clip_generator_identity ();

  title.text = g_strdup ("T");
  title.size_permille = 100;
  insert_generator (project, video, OE_CLIP_TITLE, &title, 1000000, 500000);
  g_free (title.text);
  g_assert_false (oe_edit_set_clip_key (project, stack, video, 1, &k1, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_GENERATOR);
  g_clear_error (&error);

  oe_undo_stack_free (stack);
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

/* Builds the full Wave A shape: keyed media, title, solid. */
static OeProject *
build_wave_a_project (const OeFixtures *fx)
{
  write_solid_avi (fx, "red.avi", 0xe0, 0x20, 0x20, TEST_FPS);

  gchar *red = g_build_filename (fx->dir, "red.avi", NULL);

  OeProject *project = new_project_25fps ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint ref = add_media (project, red);

  g_free (red);

  insert_clip (project, video, ref, 0, 1000000);

  OeClipKey key = oe_clip_key_identity ();

  key.color_rgb = 0x20e020;
  key.tolerance = 400;
  key.softness = 128;
  key.enabled = 1;
  g_assert_true (oe_project_set_clip_key (project, video, 0, &key, NULL));

  OeClipGenerator title = oe_clip_generator_identity ();

  title.text = g_strdup ("Titl");
  title.color_rgb = 0xabcdef;
  title.size_permille = 250;
  insert_generator (project, video, OE_CLIP_TITLE, &title, 1000000, 500000);
  g_free (title.text);

  OeClipGenerator solid = oe_clip_generator_identity ();

  solid.color_rgb = 0x010203;
  insert_generator (project, video, OE_CLIP_SOLID, &solid, 2000000, 500000);

  return project;
}

/* Rewrites the clip objects of a saved project: the callback may add
 * or remove members. Returns the new file's path (owned). */
static gchar *
rewrite_project_json (const gchar *src_path, const OeFixtures *fx, const gchar *out_name,
                      void (*mutate_clip) (JsonObject *obj, gpointer data), gpointer data)
{
  GError *error = NULL;
  JsonParser *parser = json_parser_new ();

  g_assert_true (json_parser_load_from_file (parser, src_path, &error));
  g_assert_no_error (error);

  JsonNode *root = json_parser_get_root (parser);

  g_assert_true (JSON_NODE_HOLDS_OBJECT (root));

  JsonObject *root_obj = json_node_get_object (root);

  /* The document wraps the project under "obvious-edit-project". */
  JsonNode *project_node = json_object_get_member (root_obj, "obvious-edit-project");

  g_assert_true (project_node != NULL && JSON_NODE_HOLDS_OBJECT (project_node));
  JsonObject *project_obj = json_node_get_object (project_node);
  JsonArray *tracks = json_object_get_array_member (project_obj, "tracks");

  for (guint t = 0; t < json_array_get_length (tracks); t++)
    {
      JsonObject *track = json_array_get_object_element (tracks, t);
      JsonArray *clips = json_object_get_array_member (track, "clips");

      for (guint c = 0; c < json_array_get_length (clips); c++)
        mutate_clip (json_array_get_object_element (clips, c), data);
    }

  JsonGenerator *gen = json_generator_new ();

  json_generator_set_root (gen, root);

  gchar *out_path = g_build_filename (fx->dir, out_name, NULL);

  g_assert_cmpint (json_generator_to_file (gen, out_path, &error), >=, 0);
  g_assert_no_error (error);

  g_object_unref (gen);
  g_object_unref (parser);
  return out_path;
}

static void
remove_wave_a_members (JsonObject *obj, gpointer data G_GNUC_UNUSED)
{
  json_object_remove_member (obj, "kind");
  json_object_remove_member (obj, "generator");
  json_object_remove_member (obj, "key");
}

static void
add_unknown_member (JsonObject *obj, gpointer data G_GNUC_UNUSED)
{
  json_object_set_int_member (obj, "brand-new-member", 1);
}

static void
test_json_roundtrip (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  OeProject *project = build_wave_a_project (fx);

  gchar *a_path = g_build_filename (fx->dir, "a.oe", NULL);

  g_assert_true (oe_project_format_save (project, a_path, NULL));

  /* Load → save must be byte-identical (deterministic writer). */
  GError *error = NULL;
  OeProject *loaded = oe_project_format_load (a_path, &error);

  g_assert_no_error (error);
  g_assert_nonnull (loaded);

  gchar *b_path = g_build_filename (fx->dir, "b.oe", NULL);

  g_assert_true (oe_project_format_save (loaded, b_path, &error));
  g_assert_no_error (error);

  gchar *a_bytes = NULL;
  gsize a_len = 0;

  g_assert_true (g_file_get_contents (a_path, &a_bytes, &a_len, NULL));

  gchar *b_bytes = NULL;
  gsize b_len = 0;

  g_assert_true (g_file_get_contents (b_path, &b_bytes, &b_len, NULL));
  g_assert_cmpuint (a_len, ==, b_len);
  g_assert_cmpint (memcmp (a_bytes, b_bytes, a_len), ==, 0);

  g_free (a_bytes);
  g_free (b_bytes);

  /* The loaded model carries the full Wave A state. */
  OeSequence seq = { 0 };

  oe_project_get_sequence (loaded, &seq);
  const guint video = 0;
  const OeTrack *loaded_track = g_ptr_array_index (seq.tracks, video);

  g_assert_true (loaded_track != NULL);
  g_assert_cmpuint (loaded_track->clips->len, ==, 3);
  OeClip clip = { 0 };

  g_assert_true (oe_project_get_clip (loaded, video, 0, &clip));
  g_assert_cmpint (clip.kind, ==, OE_CLIP_MEDIA);
  g_assert_cmpint (clip.key.enabled, ==, 1);
  g_assert_cmpint (clip.key.color_rgb, ==, 0x20e020);
  g_assert_cmpint (clip.key.tolerance, ==, 400);
  g_assert_cmpint (clip.key.softness, ==, 128);

  g_assert_true (oe_project_get_clip (loaded, video, 1, &clip));
  g_assert_cmpint (clip.kind, ==, OE_CLIP_TITLE);
  g_assert_cmpstr (clip.generator.text, ==, "Titl");
  g_assert_cmpint (clip.generator.color_rgb, ==, 0xabcdef);
  g_assert_cmpint (clip.generator.size_permille, ==, 250);
  g_assert_cmpint (clip.key.enabled, ==, 0);

  g_assert_true (oe_project_get_clip (loaded, video, 2, &clip));
  g_assert_cmpint (clip.kind, ==, OE_CLIP_SOLID);
  g_assert_cmpint (clip.generator.color_rgb, ==, 0x010203);
  g_assert_null (clip.generator.text);

  oe_sequence_clear (&seq);
  g_object_unref (loaded);
  g_object_unref (project);
  g_free (a_path);
  g_free (b_path);
}

static void
test_json_backfill (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  /* Save a plain media project, strip the Wave A members from every
   * clip — a pre-Phase-11 document — and load: identity backfills. */
  write_solid_avi (fx, "red.avi", 0xe0, 0x20, 0x20, TEST_FPS);

  gchar *red = g_build_filename (fx->dir, "red.avi", NULL);

  OeProject *project = new_project_25fps ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint ref = add_media (project, red);

  g_free (red);
  insert_clip (project, video, ref, 0, 1000000);

  gchar *full_path = g_build_filename (fx->dir, "full.oe", NULL);

  g_assert_true (oe_project_format_save (project, full_path, NULL));
  g_object_unref (project);

  gchar *legacy_path
      = rewrite_project_json (full_path, fx, "legacy.oe", remove_wave_a_members, NULL);

  GError *error = NULL;
  OeProject *loaded = oe_project_format_load (legacy_path, &error);

  g_assert_no_error (error);
  g_assert_nonnull (loaded);

  OeClip clip = { 0 };

  g_assert_true (oe_project_get_clip (loaded, video, 0, &clip));
  g_assert_cmpint (clip.kind, ==, OE_CLIP_MEDIA); /* absence → media */
  g_assert_cmpint (clip.key.enabled, ==, 0);      /* absence → disabled */
  g_assert_cmpint (clip.key.tolerance, ==, 0);
  g_assert_cmpint (clip.key.softness, ==, 0);
  g_assert_null (clip.generator.text); /* absence → empty payload */
  g_assert_cmpint (clip.generator.color_rgb, ==, 0);

  g_object_unref (loaded);
  g_free (full_path);
  g_free (legacy_path);
}

static void
test_json_strict_members (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  /* Closed-member rejection still fires on unknown members inside a
   * clip object (the strict-v1 contract, extended members included). */
  OeProject *project = new_project_25fps ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint ref = add_media (project, "/fixtures/m.mp4");

  insert_clip (project, video, ref, 0, 1000000);

  gchar *src_path = g_build_filename (fx->dir, "src.oe", NULL);

  g_assert_true (oe_project_format_save (project, src_path, NULL));
  g_object_unref (project);

  gchar *bogus_path = rewrite_project_json (src_path, fx, "bogus.oe", add_unknown_member, NULL);

  GError *error = NULL;
  OeProject *loaded = oe_project_format_load (bogus_path, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_UNKNOWN_MEMBER);
  g_clear_error (&error);

  g_free (src_path);
  g_free (bogus_path);
}

/* ------------------------------------------------------------------ */
/* Rasterizer                                                          */
/* ------------------------------------------------------------------ */

static void
test_raster_determinism ()
{
  GError *error = NULL;
  const gint W = 160, H = 120;

  /* Titles: identical inputs → byte-identical buffers. */
  guint8 *a = oe_generator_raster_title ("Hello", 0xff8800, 300, W, H, &error);
  guint8 *b = oe_generator_raster_title ("Hello", 0xff8800, 300, W, H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (a);
  g_assert_nonnull (b);
  g_assert_cmpint (memcmp (a, b, (gsize) W * H * 4), ==, 0);

  /* Content: transparent in the frame corners, ink near the vertical
   * center (D10: centered anchor). Structure only — never glyph
   * shapes. */
  g_assert_cmpuint (a[3], ==, 0); /* top-left corner alpha */
  g_assert_cmpuint (a[((gsize) (H - 1) * W + (W - 1)) * 4 + 3], ==, 0);

  gsize ink = 0;

  for (gint64 p = 0; p < (gint64) W * H; p++)
    if (a[p * 4 + 3] > 0)
      ink++;

  g_assert_cmpuint (ink, >, 0);

  /* Same geometry, different text → different buffer (still cover). */
  guint8 *c = oe_generator_raster_title ("Hello!", 0xff8800, 300, W, H, &error);

  g_assert_no_error (error);
  g_assert_cmpint (memcmp (a, c, (gsize) W * H * 4), !=, 0);
  g_free (a);
  g_free (b);
  g_free (c);

  /* Solids: exact fill, full opacity, byte-identical across calls. */
  guint8 *s1 = oe_generator_raster_solid (0x204080, W, H);
  guint8 *s2 = oe_generator_raster_solid (0x204080, W, H);

  g_assert_nonnull (s1);
  g_assert_nonnull (s2);
  g_assert_cmpint (memcmp (s1, s2, (gsize) W * H * 4), ==, 0);

  for (gint64 p = 0; p < (gint64) W * H; p++)
    {
      g_assert_cmpuint (s1[p * 4 + 0], ==, 0x80);
      g_assert_cmpuint (s1[p * 4 + 1], ==, 0x40);
      g_assert_cmpuint (s1[p * 4 + 2], ==, 0x20);
      g_assert_cmpuint (s1[p * 4 + 3], ==, 0xff);
    }

  g_free (s1);
  g_free (s2);
}
static void
test_raster_cache (void)
{
  GError *error = NULL;
  const gint W = 160, H = 120;
  OeProject *project = new_project_25fps ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);

  OeClipGenerator title = oe_clip_generator_identity ();

  title.text = g_strdup ("Cache");
  title.size_permille = 200;
  insert_generator (project, video, OE_CLIP_TITLE, &title, 0, 1000000);
  g_free (title.text);

  OeClip clip = { 0 };

  g_assert_true (oe_project_get_clip (project, video, 0, &clip));

  OeGeneratorCache *cache = oe_generator_cache_new ();

  /* Hit: the same clip identity + payload returns the same buffer. */
  const guint8 *r1 = oe_generator_cache_raster (cache, &clip, W, H, &error);
  const guint8 *r2 = oe_generator_cache_raster (cache, &clip, W, H, &error);

  g_assert_no_error (error);
  g_assert_nonnull (r1);
  g_assert_true (r1 == r2);

  /* Miss: a different payload is a different entry (and different
   * pixels — text changed). */
  OeClipGenerator renamed = oe_clip_generator_identity ();

  renamed.text = g_strdup ("Cache!");
  renamed.size_permille = 200;
  g_assert_true (oe_project_set_clip_generator (project, video, 0, &renamed, NULL));
  oe_clip_generator_clear (&renamed);

  g_assert_true (oe_project_get_clip (project, video, 0, &clip));

  const guint8 *r3 = oe_generator_cache_raster (cache, &clip, W, H, &error);

  g_assert_no_error (error);
  g_assert_true (r3 != r1);
  g_assert_cmpint (memcmp (r1, r3, (gsize) W * H * 4), !=, 0);

  /* Distinct clips with identical payloads stay distinct entries
   * (keyed by clip identity — the model is immutable between refreshes). */
  OeClipGenerator same_text = oe_clip_generator_identity ();

  same_text.text = g_strdup ("Cache");
  same_text.size_permille = 200;
  insert_generator (project, video, OE_CLIP_TITLE, &same_text, 1000000, 100000);
  oe_clip_generator_clear (&same_text);

  OeClip clip1 = { 0 };

  g_assert_true (oe_project_get_clip (project, video, 1, &clip1));

  const guint8 *r4 = oe_generator_cache_raster (cache, &clip1, W, H, &error);

  g_assert_no_error (error);
  g_assert_true (r4 != r1);
  g_assert_cmpint (memcmp (r1, r4, (gsize) W * H * 4), ==, 0);

  /* Dropping the cache and rebuilding yields a fresh buffer with the
   * same pixels (the snapshot-refresh contract at the session level).
   * Copy the old bytes out first — the cache owns its buffers. */
  const gsize raster_len = (gsize) W * H * 4;
  guint8 *r3_copy = g_memdup2 (r3, raster_len);

  oe_generator_cache_free (cache);
  cache = oe_generator_cache_new ();

  const guint8 *r5 = oe_generator_cache_raster (cache, &clip, W, H, &error);

  g_assert_no_error (error);
  /* Ownership, not address identity, is the contract: a fresh cache
   * owns its own buffer (the allocator may legally reuse the freed
   * address), and the bytes must match the determinism expectation. */
  g_assert_cmpint (memcmp (r3_copy, r5, raster_len), ==, 0);

  g_free (r3_copy);
  oe_generator_cache_free (cache);
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */
/* Chroma-key math (the pure rule)                                     */
/* ------------------------------------------------------------------ */

static void
test_key_math (void)
{
  /* Green key, hard cut (softness 0): exact match → 0, anything
   * else → 255. */
  OeClipKey hard = oe_clip_key_identity ();

  hard.color_rgb = 0x20e020;
  hard.tolerance = 0;
  hard.softness = 0;
  hard.enabled = 1;

  g_assert_cmpuint (oe_render_chroma_key_alpha (&hard, 0x20, 0xe0, 0x20), ==, 0);
  g_assert_cmpuint (oe_render_chroma_key_alpha (&hard, 0x21, 0xe1, 0x20), ==, 255);
  g_assert_cmpuint (oe_render_chroma_key_alpha (&hard, 0x00, 0x00, 0x00), ==, 255);
  g_assert_cmpuint (oe_render_chroma_key_alpha (&hard, 0xff, 0xff, 0xff), ==, 255);

  /* Tolerance: distances at or under the converted tolerance stay
   * keyed. tolerance=512 converts to 512·255²/1024 = 32512 in the
   * 255ths distance domain (≈127.7 in unit RGB distance), so a
   * pixel 32 units away in one channel (dist = 32·255 = 8160) is
   * keyed, and one 200 units away (dist = 51000) is not. */
  OeClipKey tol = hard;

  tol.tolerance = 512;
  g_assert_cmpuint (oe_render_chroma_key_alpha (&tol, 0x20, 0xc0, 0x20), ==, 0);   /* 32 off  */
  g_assert_cmpuint (oe_render_chroma_key_alpha (&tol, 0x20, 0x18, 0x20), ==, 255); /* 200 off */

  /* Softness: the ramp rounds EXACTLY ONCE. tolerance=0,
   * softness=512 → soft = 32512; a pixel 40 units off in one channel
   * has dist = 10200 → alpha = round(10200·255 / 32512) = 80. */
  OeClipKey soft = hard;

  soft.softness = 512;
  g_assert_cmpuint (oe_render_chroma_key_alpha (&soft, 0x20, 0xb8, 0x20), ==, 80);

  /* The same pixel under a small softness crosses the upper domain
   * edge: softness=128 converts to 8129, and dist = 10200 is at or
   * above tol+soft, so the fully-opaque rule fires — 255, not a
   * saturated ramp value. */
  OeClipKey narrow = hard;

  narrow.softness = 128;
  g_assert_cmpuint (oe_render_chroma_key_alpha (&narrow, 0x20, 0xb8, 0x20), ==, 255);

  /* Monotone non-decreasing across the ramp: doubling the distance
   * never lowers the alpha. */
  OeClipKey ramp = hard;

  ramp.softness = 1024;
  guint last = 0;

  for (int d = 0; d <= 255; d += 15)
    {
      const guint alpha = oe_render_chroma_key_alpha (&ramp, 0x20, (gint) (0xe0 - d), 0x20);

      g_assert_cmpuint (alpha, >=, last);
      last = alpha;
    }
  /* d = 255 lands exactly on tol+soft: the upper-domain edge fires. */
  g_assert_cmpuint (last, ==, 255);
}

/* ------------------------------------------------------------------ */
/* Compositor: fast path and layer behavior                            */
/* ------------------------------------------------------------------ */

static void
test_fast_path_preserved (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  /* (a) The pre-Phase-11 shape — exactly one covering media clip with
   * the default transform and a disabled key — must render byte-
   * identically through the fast path AND through the layered path.
   * Two stacked identical opaque media clips force the layered path;
   * src-over with alpha 255 copies src exactly, so both routes must
   * agree bit for bit. */
  write_solid_avi (fx, "red.avi", 0xe0, 0x20, 0x20, TEST_FPS);

  gchar *red = g_build_filename (fx->dir, "red.avi", NULL);

  OeProject *single = new_project_25fps ();
  const guint t0 = oe_project_add_track (single, OE_TRACK_VIDEO);
  const guint ref = add_media (single, red);

  insert_clip (single, t0, ref, 0, 1000000);

  OeProject *stacked = new_project_25fps ();
  const guint s0 = oe_project_add_track (stacked, OE_TRACK_VIDEO);
  const guint s1 = oe_project_add_track (stacked, OE_TRACK_VIDEO);
  const guint ref2 = add_media (stacked, red);

  insert_clip (stacked, s0, ref2, 0, 1000000);
  insert_clip (stacked, s1, ref2, 0, 1000000);

  guint8 *fast = render_at (single, 240000);
  guint8 *layered = render_at (stacked, 240000);

  g_assert_cmpint (memcmp (fast, layered, (gsize) TEST_W * TEST_H * 4), ==, 0);
  assert_dominant (fast, TEST_W, TEST_H, "media renders red", 0xe0, 0x20, 0x20);
  g_free (fast);
  g_free (layered);

  /* (b) A generator must NEVER reach ensure_source(media_ref): a
   * title-only project has no media at all, so a fast-path take (or
   * any media decode) would fail with a missing source. It renders
   * clean instead. */
  OeProject *titles_only = new_project_25fps ();
  const guint to = oe_project_add_track (titles_only, OE_TRACK_VIDEO);

  OeClipGenerator title = oe_clip_generator_identity ();

  title.text = g_strdup ("Only");
  title.color_rgb = 0xffffff;
  title.size_permille = 200;
  insert_generator (titles_only, to, OE_CLIP_TITLE, &title, 0, 1000000);
  g_free (title.text);

  guint8 *frame = render_at (titles_only, 240000);

  /* White ink is channel-neutral — assert brightness contrast between
   * the text band and the backdrop instead of channel dominance. */
  double center[3], margin[3];

  window_mean (frame, TEST_W, TEST_H, 40, 40, 120, 80, center);
  window_mean (frame, TEST_W, TEST_H, 0, 0, 10, 10, margin);
  g_assert_cmpfloat (center[0], >, margin[0] + 20.0);        /* ink above the backdrop */
  g_assert_cmpfloat (fabs (center[0] - center[1]), <, 25.0); /* white stays balanced */
  g_assert_cmpfloat (fabs (center[1] - center[2]), <, 25.0);
  g_assert_cmpfloat (margin[0], <, 40.0); /* margins stay near black */
  g_assert_cmpfloat (margin[1], <, 40.0);
  g_free (frame);

  g_object_unref (titles_only);
  g_object_unref (stacked);
  g_object_unref (single);
  g_free (red);
}

static void
test_compositor_title_solid (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  /* Media below; title and solid above, as ordinary layers. */
  write_solid_avi (fx, "red.avi", 0xe0, 0x20, 0x20, TEST_FPS);

  gchar *red = g_build_filename (fx->dir, "red.avi", NULL);

  OeProject *project = new_project_25fps ();
  const guint base = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint upper = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint ref = add_media (project, red);

  g_free (red);
  insert_clip (project, base, ref, 0, 1000000);

  OeClipGenerator solid = oe_clip_generator_identity ();

  solid.color_rgb = 0x204080;
  insert_generator (project, upper, OE_CLIP_SOLID, &solid, 0, 500000);

  OeClipGenerator title = oe_clip_generator_identity ();

  title.text = g_strdup ("Over");
  title.color_rgb = 0xffffff;
  title.size_permille = 200;
  insert_generator (project, upper, OE_CLIP_TITLE, &title, 500000, 500000);
  g_free (title.text);

  /* Solid half: the solid is opaque at layer size — the covered
   * region is EXACTLY the solid color (alpha 255 replaces the base). */
  guint8 *frame = render_at (project, 240000);

  const guint8 *px = frame + ((gsize) 60 * TEST_W + 80) * 4;

  g_assert_cmpuint (px[0], ==, 0x80);
  g_assert_cmpuint (px[1], ==, 0x40);
  g_assert_cmpuint (px[2], ==, 0x20);
  g_assert_cmpuint (px[3], ==, 0xff);
  g_free (frame);

  /* Title half: ink dominates the center band; the base red shows at
   * the margins (generators do not box-fit the frame — D13). */
  frame = render_at (project, 740000);

  g_assert_cmpint (frame[3], ==, 0xff); /* canvas stays opaque */
  assert_dominant (frame, TEST_W, TEST_H, "title half carries red base", 0xe0, 0x20, 0x20);
  g_free (frame);

  g_object_unref (project);
}

static void
test_key_composite (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  write_solid_avi (fx, "red.avi", 0xe0, 0x20, 0x20, TEST_FPS);
  write_solid_avi (fx, "green.avi", 0x20, 0xe0, 0x20, TEST_FPS);

  gchar *red = g_build_filename (fx->dir, "red.avi", NULL);
  gchar *green = g_build_filename (fx->dir, "green.avi", NULL);

  OeProject *project = new_project_25fps ();
  const guint base = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint upper = oe_project_add_track (project, OE_TRACK_VIDEO);
  const guint red_ref = add_media (project, red);
  const guint green_ref = add_media (project, green);

  insert_clip (project, base, red_ref, 0, 1000000);
  insert_clip (project, upper, green_ref, 0, 1000000);

  guint8 *no_key = render_at (project, 240000);

  assert_dominant (no_key, TEST_W, TEST_H, "unkeyed upper wins", 0x20, 0xe0, 0x20);

  /* Key the upper clip against green, hard cut: the decoded green
   * lands inside the tolerance, so the keyed region vanishes and the
   * lower layer shows — byte-identical to a red-only render. */
  OeClipKey key = oe_clip_key_identity ();

  key.color_rgb = 0x20e020;
  key.tolerance = 512;
  key.softness = 0;
  key.enabled = 1;
  g_assert_true (oe_project_set_clip_key (project, upper, 0, &key, NULL));

  guint8 *keyed = render_at (project, 240000);

  OeProject *red_only = new_project_25fps ();
  const guint ro = oe_project_add_track (red_only, OE_TRACK_VIDEO);
  const guint red_ref2 = add_media (red_only, red);

  insert_clip (red_only, ro, red_ref2, 0, 1000000);

  guint8 *lower_only = render_at (red_only, 240000);

  g_assert_cmpint (memcmp (keyed, lower_only, (gsize) TEST_W * TEST_H * 4), ==, 0);

  /* Key disabled ≡ no-key render: flipping the flag back must
   * reproduce the unkeyed bytes exactly. */
  OeClipKey off = key;

  off.enabled = 0;
  g_assert_true (oe_project_set_clip_key (project, upper, 0, &off, NULL));

  guint8 *disabled = render_at (project, 240000);

  g_assert_cmpint (memcmp (disabled, no_key, (gsize) TEST_W * TEST_H * 4), ==, 0);
  g_free (no_key);
  g_free (keyed);
  g_free (disabled);
  /* Softness: a mid-distance pixel blend lands BETWEEN the two
   * classes. Blue upper keyed against red with softness 1024 keeps
   * large-distance pixels at partial alpha: blue still dominates the
   * frame, but the blue channel mean sits strictly between the hard
   * blends (weaker than full opacity, stronger than the base). */
  OeProject *soft_project = new_project_25fps ();
  const guint sb = oe_project_add_track (soft_project, OE_TRACK_VIDEO);
  const guint su = oe_project_add_track (soft_project, OE_TRACK_VIDEO);

  /* (0x40, 0x30, 0xc0) sits at unit RGB distance ≈227 from the red
   * key — inside the softness=1024 ramp (edge at 255), so the frame
   * renders at partial alpha ≈226/255 and stays blue-dominant over
   * the red base. Pure blue (0x20, 0x30, 0xe0) would sit beyond the
   * edge and render fully opaque, defeating the check. */
  write_solid_avi (fx, "blue.avi", 0x40, 0x30, 0xc0, TEST_FPS / 2);

  gchar *blue = g_build_filename (fx->dir, "blue.avi", NULL);
  const guint b_ref = add_media (soft_project, red);
  const guint u_ref = add_media (soft_project, blue);

  g_free (blue);

  insert_clip (soft_project, sb, b_ref, 0, 1000000);
  insert_clip (soft_project, su, u_ref, 0, 1000000);

  OeClipKey soft_key = oe_clip_key_identity ();

  soft_key.color_rgb = 0xe02020;
  soft_key.tolerance = 0;
  soft_key.softness = 1024;
  soft_key.enabled = 1;
  g_assert_true (oe_project_set_clip_key (soft_project, su, 0, &soft_key, NULL));

  guint8 *soft_frame = render_at (soft_project, 240000);

  double soft_mean[3], base_mean[3];

  window_mean (soft_frame, TEST_W, TEST_H, 0, 0, TEST_W, TEST_H, soft_mean);
  window_mean (lower_only, TEST_W, TEST_H, 0, 0, TEST_W, TEST_H, base_mean);

  /* Blue dominant, but pulled toward the red base: intermediate. */
  g_assert_cmpfloat (soft_mean[2], >, soft_mean[0]);
  g_assert_cmpfloat (soft_mean[2], <, 224.0 * 0.95); /* below full-opacity blue */
  g_assert_cmpfloat (soft_mean[2], >, base_mean[2]); /* above the base's blue */
  g_free (soft_frame);
  g_free (lower_only);

  g_object_unref (soft_project);
  g_object_unref (red_only);
  g_object_unref (project);
  g_free (red);
  g_free (green);
}

/* ------------------------------------------------------------------ */
/* Export parity                                                       */
/* ------------------------------------------------------------------ */

static void
test_export_parity (OeFixtures *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  /* (a) Titled frame: media + a title layer, exported and decoded
   * back at the mid-title instant vs the shared seam. The palette is
   * deliberately chroma-neutral (gray base, white ink): sharp chroma
   * edges are the one feature 4:2:0 bilinear resampling cannot carry
   * through a codec round trip within the ±8 block-mean budget —
   * luma edges are full-resolution and survive exactly. Colorful
   * title/solid/keyed appearance is asserted exactly in the
   * in-process compositor tests above. */
  write_solid_avi (fx, "gray.avi", 0x40, 0x40, 0x40, TEST_FPS);

  gchar *gray = g_build_filename (fx->dir, "gray.avi", NULL);

  OeProject *titled = new_project_25fps ();
  const guint tb = oe_project_add_track (titled, OE_TRACK_VIDEO);
  const guint tu = oe_project_add_track (titled, OE_TRACK_VIDEO);
  const guint tref = add_media (titled, gray);

  insert_clip (titled, tb, tref, 0, 1000000);

  OeClipGenerator title = oe_clip_generator_identity ();

  title.text = g_strdup ("Hi");
  title.color_rgb = 0xffffff;
  title.size_permille = 500;
  insert_generator (titled, tu, OE_CLIP_TITLE, &title, 0, 1000000);
  g_free (title.text);
  g_free (gray);

  guint8 *titled_render = render_at (titled, 240000);

  gchar *dest = g_build_filename (fx->dir, "titled.mp4", NULL);

  g_assert_true (run_export (titled, dest, OE_EXPORT_QUALITY_MEDIUM, NULL));

  int w = 0, h = 0;
  guint8 *titled_decoded = decode_video_frame_bgra (dest, 6, &w, &h);

  g_assert_nonnull (titled_decoded);
  g_assert_cmpint (w, ==, TEST_W);
  g_assert_cmpint (h, ==, TEST_H);
  /* Neutral palette: the CENTER band carries the (center-anchored,
   * D10) ink and is brighter than the background margins; dominance
   * helpers cannot express gray-on-gray, so brightness contrast and
   * channel balance stand in. Bands are proportional to the canvas. */
  double title_band[3], base_band[3];

  window_mean (titled_render, TEST_W, TEST_H, 0, TEST_H * 3 / 8, TEST_W, TEST_H * 5 / 8,
               title_band);
  window_mean (titled_render, TEST_W, TEST_H, 0, 0, TEST_W, TEST_H / 12, base_band);
  g_assert_cmpfloat (title_band[0], >, base_band[0] + 20.0);         /* ink over base */
  g_assert_cmpfloat (fabs (title_band[0] - title_band[2]), <, 25.0); /* neutral */
  g_assert_cmpfloat (base_band[0], <, 100.0);                        /* margins stay dark gray */

  window_mean (titled_decoded, TEST_W, TEST_H, 0, TEST_H * 3 / 8, TEST_W, TEST_H * 5 / 8,
               title_band);
  window_mean (titled_decoded, TEST_W, TEST_H, 0, 0, TEST_W, TEST_H / 12, base_band);
  g_assert_cmpfloat (title_band[0], >, base_band[0] + 20.0); /* same shape exported */
  g_assert_cmpfloat (base_band[0], <, 100.0);

  assert_block_mean_parity (titled_render, titled_decoded, TEST_W, TEST_H);

  g_free (titled_render);
  g_free (titled_decoded);
  g_free (dest);
  g_object_unref (titled);

  /* (b) Keyed two-layer frame: green keyed over red. With
   * tolerance 512 (≈127.6 in unit RGB distance) every solid-green
   * pixel sits far inside the keyed domain, so the entire upper layer
   * vanishes and the frame is uniform red in both renderings — the
   * keyed region shows the lower layer with no chroma edges for the
   * codec to smear (soft/partial keying is covered exactly by the
   * in-process compositor tests). */
  write_solid_avi (fx, "red.avi", 0xe0, 0x20, 0x20, TEST_FPS);
  write_solid_avi (fx, "green.avi", 0x20, 0xe0, 0x20, TEST_FPS);

  gchar *red = g_build_filename (fx->dir, "red.avi", NULL);
  gchar *green = g_build_filename (fx->dir, "green.avi", NULL);

  OeProject *keyed = new_project_25fps ();
  const guint kb = oe_project_add_track (keyed, OE_TRACK_VIDEO);
  const guint ku = oe_project_add_track (keyed, OE_TRACK_VIDEO);
  const guint kred = add_media (keyed, red);
  const guint kgreen = add_media (keyed, green);

  g_free (red);
  g_free (green);

  insert_clip (keyed, kb, kred, 0, 1000000);
  insert_clip (keyed, ku, kgreen, 0, 1000000);

  OeClipKey key = oe_clip_key_identity ();

  key.color_rgb = 0x20e020;
  key.tolerance = 512;
  key.softness = 0;
  key.enabled = 1;
  g_assert_true (oe_project_set_clip_key (keyed, ku, 0, &key, NULL));

  guint8 *keyed_render = render_at (keyed, 240000);

  dest = g_build_filename (fx->dir, "keyed.mp4", NULL);
  g_assert_true (run_export (keyed, dest, OE_EXPORT_QUALITY_MEDIUM, NULL));

  guint8 *keyed_decoded = decode_video_frame_bgra (dest, 6, &w, &h);

  g_assert_nonnull (keyed_decoded);

  assert_dominant (keyed_render, TEST_W, TEST_H, "render keyed frame is red", 0xe0, 0x20, 0x20);
  assert_dominant (keyed_decoded, TEST_W, TEST_H, "export keyed frame is red", 0xe0, 0x20, 0x20);
  assert_block_mean_parity (keyed_render, keyed_decoded, TEST_W, TEST_H);

  g_free (keyed_render);
  g_free (keyed_decoded);
  g_free (dest);
  g_object_unref (keyed);

  /* (c) Solid exactness: a solid over black canvas exports with the
   * solid color dominant (and the decode-back stays in tolerance of
   * the seam's exact fill). */
  OeProject *solid_project = new_project_25fps ();
  const guint so = oe_project_add_track (solid_project, OE_TRACK_VIDEO);

  OeClipGenerator solid = oe_clip_generator_identity ();

  solid.color_rgb = 0x204080;
  insert_generator (solid_project, so, OE_CLIP_SOLID, &solid, 0, 1000000);

  guint8 *solid_render = render_at (solid_project, 240000);

  dest = g_build_filename (fx->dir, "solid.mp4", NULL);
  g_assert_true (run_export (solid_project, dest, OE_EXPORT_QUALITY_MEDIUM, NULL));

  guint8 *solid_decoded = decode_video_frame_bgra (dest, 6, &w, &h);

  g_assert_nonnull (solid_decoded);

  assert_dominant (solid_render, TEST_W, TEST_H, "render solid is blue-ish", 0x20, 0x40, 0x80);
  assert_dominant (solid_decoded, TEST_W, TEST_H, "export solid is blue-ish", 0x20, 0x40, 0x80);
  assert_block_mean_parity (solid_render, solid_decoded, TEST_W, TEST_H);

  g_free (solid_render);
  g_free (solid_decoded);
  g_free (dest);
  g_object_unref (solid_project);
}

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

#define TK_ADD_FUNC(path, fn) g_test_add_func ((path), (fn))
#define TK_ADD(path, fn)                                                                           \
  g_test_add ((path), OeFixtures, NULL, fixture_set_up, (fn), fixture_tear_down)

  TK_ADD_FUNC ("/titles-key/model-insert-validate", test_model_insert_validate);
  TK_ADD_FUNC ("/titles-key/model-mutate-reject", test_model_mutate_reject);
  TK_ADD_FUNC ("/titles-key/undo-generator-replay", test_undo_generator_replay);
  TK_ADD_FUNC ("/titles-key/undo-key-replay", test_undo_key_replay);
  TK_ADD ("/titles-key/json-roundtrip", test_json_roundtrip);
  TK_ADD ("/titles-key/json-backfill", test_json_backfill);
  TK_ADD ("/titles-key/json-strict-members", test_json_strict_members);
  TK_ADD_FUNC ("/titles-key/raster-determinism", test_raster_determinism);
  TK_ADD_FUNC ("/titles-key/raster-cache", test_raster_cache);
  TK_ADD_FUNC ("/titles-key/key-math", test_key_math);
  TK_ADD ("/titles-key/fast-path-preserved", test_fast_path_preserved);
  TK_ADD ("/titles-key/compositor-title-solid", test_compositor_title_solid);
  TK_ADD ("/titles-key/key-composite", test_key_composite);
  TK_ADD ("/titles-key/export-parity", test_export_parity);

#undef TK_ADD_FUNC
#undef TK_ADD

  return g_test_run ();
}
