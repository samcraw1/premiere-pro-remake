/* oe_probe.h — stream metadata probing (Phase 2).
 *
 * The metadata contract for imported media. Every field is integer-valued:
 * duration is integer microseconds and frame rate is a num/den rational
 * pair, honoring the project-format time-model floor (no float seconds in
 * APIs that could leak into serialized state).
 *
 * GTK-free, FFmpeg-only: this module lives in the media layer per the
 * adapter leak rule (oe_ffmpeg.h) — no GTK/GDK header may be included
 * from here, and no FFmpeg header may leak past this file.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * OeMediaKind:
 * @OE_MEDIA_KIND_VIDEO: a moving video stream (with or without audio).
 * @OE_MEDIA_KIND_AUDIO: an audio-only file.
 * @OE_MEDIA_KIND_STILL_IMAGE: a single still image.
 */
typedef enum
{
  OE_MEDIA_KIND_VIDEO,
  OE_MEDIA_KIND_AUDIO,
  OE_MEDIA_KIND_STILL_IMAGE,
} OeMediaKind;

const gchar *oe_media_kind_get_name (OeMediaKind kind);

/**
 * OeProbeInfo:
 * @kind: coarse media kind
 * @container_name: FFmpeg container (demuxer) name, e.g. "avi", "wav"
 * @duration_us: integer microseconds; 0 when not applicable (stills)
 * @width: video/still width in pixels; 0 for audio
 * @height: video/still height in pixels; 0 for audio
 * @frame_rate_num: rational frame rate numerator; 0/0 when not applicable
 * @frame_rate_den: rational frame rate denominator
 * @sample_rate: audio sample rate in Hz; 0 when no audio stream
 * @channels: audio channel count; 0 when no audio stream
 * @video_codec: codec name like "mjpeg", "png", or NULL
 * @audio_codec: codec name like "pcm_s16le", "aac", or NULL
 *
 * Zero-initialized with oe_probe_info_init(), freed with
 * oe_probe_info_clear().
 */
typedef struct
{
  OeMediaKind kind;
  gchar *container_name;
  gint64 duration_us;
  gint width;
  gint height;
  gint frame_rate_num;
  gint frame_rate_den;
  gint sample_rate;
  gint channels;
  gchar *video_codec;
  gchar *audio_codec;
} OeProbeInfo;

/**
 * oe_probe_info_init:
 * @info: uninitialised record
 *
 * Zeroes every field; safe to call repeatedly.
 */
void oe_probe_info_init (OeProbeInfo *info);

/**
 * oe_probe_info_clear:
 * @info: record previously probed or initialised
 *
 * Frees owned strings and zeroes the record. Safe on a zeroed record.
 */
void oe_probe_info_clear (OeProbeInfo *info);
/**
 * oe_probe_info_copy:
 * @dst: destination record (cleared first)
 * @src: source record
 *
 * Deep-copies owned strings so records can move between layers
 * (worker → library → UI) without sharing ownership.
 */
void oe_probe_info_copy (OeProbeInfo *dst, const OeProbeInfo *src);


/**
 * OE_PROBE_ERROR: error domain for probing failures.
 */
#define OE_PROBE_ERROR (oe_probe_error_quark ())

GQuark oe_probe_error_quark (void);

/**
 * OeProbeError:
 * @OE_PROBE_ERROR_OPEN_FAILED: missing, unreadable, or not a media
 *   container at all.
 * @OE_PROBE_ERROR_UNSUPPORTED: opened fine, but holds no decodable
 *   audio/video stream.
 */
typedef enum
{
  OE_PROBE_ERROR_OPEN_FAILED,
  OE_PROBE_ERROR_UNSUPPORTED,
} OeProbeError;

/**
 * oe_probe_file:
 * @path: file to probe
 * @info: record to fill (must be zeroed or cleared first)
 * @error: return location for a #GError, or NULL to ignore
 *
 * Reads the container metadata of @path without decoding frames. On
 * success every field of @info describes the file's primary streams.
 * An unopenable or unidentifiable file yields OE_PROBE_ERROR_OPEN_FAILED;
 * a container with no decodable audio/video stream yields
 * OE_PROBE_ERROR_UNSUPPORTED. On failure @info is left cleared.
 *
 * Returns: TRUE on success.
 */
gboolean oe_probe_file (const gchar *path, OeProbeInfo *info, GError **error);

G_END_DECLS
