/* fixture_media.h — runtime media fixture generator for GTK-free tests.
 *
 * Phase 2 has no ffmpeg CLI in the environment, so tests generate real
 * media in-process through libavformat/libavcodec into a g_dir_make_tmp
 * directory. Compiled into test executables only (see meson.build); never
 * linked into the application binary. Every fixture is written with known
 * parameters so test_probe can assert exact metadata and test_media_jobs
 * can assert exact box-fit geometry.
 *
 * Known parameters (pinned here, asserted in the tests):
 *
 *   WAV   pcm_s16le, OE_FIXTURE_WAV_RATE Hz, OE_FIXTURE_WAV_CHANNELS
 *         channel(s), OE_FIXTURE_WAV_DURATION_US of a sine sweep.
 *   AVI   MJPEG video only, OE_FIXTURE_AVI_WIDTH x OE_FIXTURE_AVI_HEIGHT,
 *         OE_FIXTURE_AVI_FPS_NUM/OE_FIXTURE_AVI_FPS_DEN fps,
 *         OE_FIXTURE_AVI_FRAMES frames (≈ OE_FIXTURE_AVI_DURATION_US).
 *   PNG   a self-contained still image, OE_FIXTURE_PNG_WIDTH x
 *         OE_FIXTURE_PNG_HEIGHT (written directly from the encoder packet).
 *   TXT   plain text — never a media container.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* WAV fixture parameters. */
#define OE_FIXTURE_WAV_RATE 22050
#define OE_FIXTURE_WAV_CHANNELS 1
#define OE_FIXTURE_WAV_DURATION_US 500000 /* 0.5 s of mono samples */

/* AVI fixture parameters (MJPEG video, no audio track). */
/* Larger than OE_THUMBNAIL_BOX so the thumbnail job exercises the
 * downscale path; the PNG fixture stays below it for the no-upscale path. */
#define OE_FIXTURE_AVI_WIDTH 192
#define OE_FIXTURE_AVI_HEIGHT 108
#define OE_FIXTURE_AVI_FPS_NUM 25
#define OE_FIXTURE_AVI_FPS_DEN 1
#define OE_FIXTURE_AVI_FRAMES 30
#define OE_FIXTURE_AVI_DURATION_US 1200000 /* 30 / 25 s */

/* PNG fixture parameters. */
#define OE_FIXTURE_PNG_WIDTH 32
#define OE_FIXTURE_PNG_HEIGHT 20

/* The set of fixtures one test run works with. Paths are owned and freed
 * by oe_fixtures_free(). */
typedef struct
{
  gchar *dir;        /* scratch directory, removed by oe_fixtures_free() */
  gchar *wav_path;   /* audio fixture                                    */
  gchar *avi_path;   /* video fixture                                    */
  gchar *png_path;   /* still-image fixture                              */
  gchar *txt_path;   /* garbage (plain text)                             */
  gchar *empty_path; /* zero bytes                                       */
} OeFixtures;

/**
 * oe_fixtures_create:
 * @fx: fixture set to fill
 * @error: return location for a #GError, or NULL to ignore
 *
 * Creates a temporary directory and generates every fixture into it.
 * Fails the test framework (g_assert) on internal generation errors;
 * @error covers directory creation and I/O failures.
 *
 * Returns: TRUE on success.
 */
gboolean oe_fixtures_create (OeFixtures *fx, GError **error);

/**
 * oe_fixtures_free:
 * @fx: fixture set previously filled by oe_fixtures_create()
 *
 * Removes the scratch directory (recursively) and resets the struct.
 */
void oe_fixtures_free (OeFixtures *fx);

G_END_DECLS
