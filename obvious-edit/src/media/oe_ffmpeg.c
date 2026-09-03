#include "oe_ffmpeg.h"

#include "../app/oe_log.h"

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/*
 * Phase 0 runs the lifecycle on the main thread only, so a plain static flag
 * is deliberate. When a second thread arrives, this becomes a g_once-guarded
 * section without touching callers.
 */
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

gboolean
oe_ffmpeg_init (GError **error)
{
  int rv;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (ffmpeg_initialized)
    {
      oe_log (OE_LOG_LEVEL_DEBUG, "ffmpeg adapter already initialised");
      return TRUE;
    }

  rv = avformat_network_init ();
  if (rv != 0)
    {
      g_set_error (error, OE_FFMPEG_ERROR, OE_FFMPEG_ERROR_INIT_FAILED,
                   "avformat_network_init failed with code %d", rv);
      return FALSE;
    }

  ffmpeg_initialized = TRUE;
  oe_ffmpeg_log_versions ();
  oe_log (OE_LOG_LEVEL_DEBUG, "ffmpeg adapter initialised");
  return TRUE;
}

void
oe_ffmpeg_shutdown (void)
{
  if (!ffmpeg_initialized)
    return;

  avformat_network_deinit ();
  ffmpeg_initialized = FALSE;
  oe_log (OE_LOG_LEVEL_DEBUG, "ffmpeg adapter shut down");
}

gboolean
oe_ffmpeg_is_initialized (void)
{
  return ffmpeg_initialized;
}
