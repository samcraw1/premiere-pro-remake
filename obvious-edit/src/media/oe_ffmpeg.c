#include "oe_ffmpeg.h"

#include "../app/oe_log.h"

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/*
 * Phase 2 brings a second thread (the import worker), so the one-time
 * libavformat setup runs under g_once: concurrent first use initialises
 * exactly once and every caller sees the same result. Shutdown re-arms
 * the guard under the state lock so a later lifecycle cycle (tests re-init;
 * the app itself shuts down once) can initialise again. That reset is safe
 * because teardown order joins the worker before shutdown runs.
 */
typedef struct
{
  int rv;
} FfmpegInitResult;

static GOnce ffmpeg_init_once = G_ONCE_INIT;
static FfmpegInitResult ffmpeg_init_result;
static GMutex ffmpeg_state_lock;
static gboolean ffmpeg_initialized = FALSE;

GQuark
oe_ffmpeg_error_quark (void)
{
  return g_quark_from_static_string ("oe-ffmpeg-error");
}

static void
oe_ffmpeg_log_versions (void)
{
  unsigned avformat = avformat_version ();
  unsigned avcodec = avcodec_version ();
  unsigned avutil = avutil_version ();
  unsigned avfilter = avfilter_version ();
  unsigned swscale = swscale_version ();
  unsigned swresample = swresample_version ();

  oe_log (OE_LOG_LEVEL_INFO,
          "FFmpeg %s linked: libavformat %u.%u.%u, libavcodec %u.%u.%u, "
          "libavutil %u.%u.%u, libavfilter %u.%u.%u, libswscale %u.%u.%u, "
          "libswresample %u.%u.%u",
          av_version_info (), AV_VERSION_MAJOR (avformat), AV_VERSION_MINOR (avformat),
          AV_VERSION_MICRO (avformat), AV_VERSION_MAJOR (avcodec), AV_VERSION_MINOR (avcodec),
          AV_VERSION_MICRO (avcodec), AV_VERSION_MAJOR (avutil), AV_VERSION_MINOR (avutil),
          AV_VERSION_MICRO (avutil), AV_VERSION_MAJOR (avfilter), AV_VERSION_MINOR (avfilter),
          AV_VERSION_MICRO (avfilter), AV_VERSION_MAJOR (swscale), AV_VERSION_MINOR (swscale),
          AV_VERSION_MICRO (swscale), AV_VERSION_MAJOR (swresample), AV_VERSION_MINOR (swresample),
          AV_VERSION_MICRO (swresample));
}

static gpointer
ffmpeg_init_run (gpointer data G_GNUC_UNUSED)
{
  ffmpeg_init_result.rv = avformat_network_init ();
  return &ffmpeg_init_result;
}

gboolean
oe_ffmpeg_init (GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  FfmpegInitResult *r = g_once (&ffmpeg_init_once, ffmpeg_init_run, NULL);

  if (r->rv != 0)
    {
      /* A hard libavformat failure stays terminal: the once guard keeps
       * reporting the same result instead of re-running the setup. */
      g_set_error (error, OE_FFMPEG_ERROR, OE_FFMPEG_ERROR_INIT_FAILED,
                   "avformat_network_init failed with code %d", r->rv);
      return FALSE;
    }

  g_mutex_lock (&ffmpeg_state_lock);
  gboolean first = !ffmpeg_initialized;
  ffmpeg_initialized = TRUE;
  g_mutex_unlock (&ffmpeg_state_lock);

  if (first)
    {
      oe_ffmpeg_log_versions ();
      oe_log (OE_LOG_LEVEL_DEBUG, "ffmpeg adapter initialised");
    }
  else
    {
      oe_log (OE_LOG_LEVEL_DEBUG, "ffmpeg adapter already initialised");
    }

  return TRUE;
}

void
oe_ffmpeg_shutdown (void)
{
  g_mutex_lock (&ffmpeg_state_lock);

  if (!ffmpeg_initialized)
    {
      g_mutex_unlock (&ffmpeg_state_lock);
      return;
    }

  avformat_network_deinit ();
  ffmpeg_initialized = FALSE;
  ffmpeg_init_once = (GOnce) G_ONCE_INIT;
  g_mutex_unlock (&ffmpeg_state_lock);

  oe_log (OE_LOG_LEVEL_DEBUG, "ffmpeg adapter shut down");
}

gboolean
oe_ffmpeg_is_initialized (void)
{
  g_mutex_lock (&ffmpeg_state_lock);
  gboolean initialized = ffmpeg_initialized;
  g_mutex_unlock (&ffmpeg_state_lock);

  return initialized;
}
