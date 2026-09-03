/* oe_probe.c — stream metadata probing implementation (Phase 2).
 *
 * Reads container metadata through libavformat only; no frames are
 * decoded here. All FFmpeg identifiers stay in this file (adapter leak
 * rule, oe_ffmpeg.h).
 */

#include "oe_probe.h"

#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include "../app/oe_log.h"

GQuark
oe_probe_error_quark (void)
{
  return g_quark_from_static_string ("oe-probe-error");
}

const gchar *
oe_media_kind_get_name (OeMediaKind kind)
{
  switch (kind)
    {
    case OE_MEDIA_KIND_VIDEO:
      return "Video";
    case OE_MEDIA_KIND_AUDIO:
      return "Audio";
    case OE_MEDIA_KIND_STILL_IMAGE:
      return "Still image";
    default:
      return "Unknown";
    }
}

void
oe_probe_info_init (OeProbeInfo *info)
{
  memset (info, 0, sizeof (*info));
}

void
oe_probe_info_copy (OeProbeInfo *dst, const OeProbeInfo *src)
{
  oe_probe_info_clear (dst);
  dst->kind = src->kind;
  dst->duration_us = src->duration_us;
  dst->width = src->width;
  dst->height = src->height;
  dst->frame_rate_num = src->frame_rate_num;
  dst->frame_rate_den = src->frame_rate_den;
  dst->sample_rate = src->sample_rate;
  dst->channels = src->channels;
  dst->container_name = g_strdup (src->container_name);
  dst->video_codec = g_strdup (src->video_codec);
  dst->audio_codec = g_strdup (src->audio_codec);
}

void
oe_probe_info_clear (OeProbeInfo *info)
{
  g_clear_pointer (&info->container_name, g_free);
  g_clear_pointer (&info->video_codec, g_free);
  g_clear_pointer (&info->audio_codec, g_free);
  memset (info, 0, sizeof (*info));
}

/* A stream counts only when a decoder exists for it in this build; an
 * attached picture (e.g. album art) is not the file's video content. */
static gboolean
stream_is_usable (AVStream *st)
{
  const AVCodecParameters *par = st->codecpar;

  if (par == NULL || (st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0)
    return FALSE;

  if (avcodec_find_decoder (par->codec_id) == NULL)
    return FALSE;

  return par->codec_type == AVMEDIA_TYPE_VIDEO || par->codec_type == AVMEDIA_TYPE_AUDIO;
}

static gint64
stream_duration_us (const AVFormatContext *fmt, const AVStream *st)
{
  if (st->duration != AV_NOPTS_VALUE && st->duration >= 0)
    return av_rescale_q (st->duration, st->time_base, AV_TIME_BASE_Q);

  if (fmt->duration != AV_NOPTS_VALUE && fmt->duration >= 0)
    return fmt->duration; /* AV_TIME_BASE is microseconds. */

  return 0;
}

static void
fill_probe_info (OeProbeInfo *info, AVFormatContext *fmt)
{
  AVStream *video = NULL;
  AVStream *audio = NULL;

  for (unsigned i = 0; i < fmt->nb_streams; i++)
    {
      AVStream *st = fmt->streams[i];

      if (!stream_is_usable (st))
        continue;

      if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video == NULL)
        video = st;
      else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio == NULL)
        audio = st;
    }

  if (video == NULL && audio == NULL)
    return; /* caller maps this to UNSUPPORTED */

  info->container_name = g_strdup (fmt->iformat != NULL ? fmt->iformat->name : "unknown");
  info->duration_us
      = video != NULL ? stream_duration_us (fmt, video) : stream_duration_us (fmt, audio);

  if (video != NULL)
    {
      const AVCodecParameters *par = video->codecpar;
      AVRational rate = av_guess_frame_rate (fmt, video, NULL);

      info->width = par->width;
      info->height = par->height;

      /* Rational in, rational out: an invalid guess stays 0/0. */
      if (rate.num > 0 && rate.den > 0)
        {
          info->frame_rate_num = rate.num;
          info->frame_rate_den = rate.den;
        }

      info->video_codec = g_strdup (avcodec_get_name (par->codec_id));

      /* A single still is a video stream with no audio and no running
       * time; anything with audio or a duration is a video. */
      if (audio == NULL && info->duration_us <= 0)
        info->kind = OE_MEDIA_KIND_STILL_IMAGE;
      else
        info->kind = OE_MEDIA_KIND_VIDEO;
    }

  if (audio != NULL)
    {
      const AVCodecParameters *par = audio->codecpar;

      info->sample_rate = par->sample_rate;
      info->channels = par->ch_layout.nb_channels;
      info->audio_codec = g_strdup (avcodec_get_name (par->codec_id));

      if (video == NULL)
        info->kind = OE_MEDIA_KIND_AUDIO;
    }
}

gboolean
oe_probe_file (const gchar *path, OeProbeInfo *info, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  g_return_val_if_fail (info != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  AVFormatContext *fmt = NULL;
  int rv = avformat_open_input (&fmt, path, NULL, NULL);

  if (rv != 0)
    {
      gchar buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };

      av_strerror (rv, buf, sizeof (buf));
      g_set_error (error, OE_PROBE_ERROR, OE_PROBE_ERROR_OPEN_FAILED, "cannot open '%s': %s", path,
                   buf);
      return FALSE;
    }

  rv = avformat_find_stream_info (fmt, NULL);

  gboolean found = FALSE;

  if (rv == 0)
    {
      for (unsigned i = 0; i < fmt->nb_streams && !found; i++)
        found = stream_is_usable (fmt->streams[i]);
    }

  if (rv != 0 || !found)
    {
      g_set_error (error, OE_PROBE_ERROR, OE_PROBE_ERROR_UNSUPPORTED,
                   "'%s' holds no decodable audio or video stream", path);
      avformat_close_input (&fmt);
      oe_probe_info_clear (info);
      return FALSE;
    }

  oe_probe_info_clear (info);
  fill_probe_info (info, fmt);

  if (info->container_name == NULL)
    {
      /* fill_probe_info found no usable stream after all: treat as
       * unsupported rather than half-filled metadata. */
      g_set_error (error, OE_PROBE_ERROR, OE_PROBE_ERROR_UNSUPPORTED,
                   "'%s' holds no decodable audio or video stream", path);
      avformat_close_input (&fmt);
      oe_probe_info_clear (info);
      return FALSE;
    }

  avformat_close_input (&fmt);

  oe_log (OE_LOG_LEVEL_DEBUG, "probed '%s': %s, %s, %s/%s", path, info->container_name,
          oe_media_kind_get_name (info->kind), info->video_codec != NULL ? info->video_codec : "-",
          info->audio_codec != NULL ? info->audio_codec : "-");
  return TRUE;
}
