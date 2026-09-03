/* oe_media_jobs.h — off-thread decode jobs (Phase 2).
 *
 * The decode half of the media layer: one video frame scaled to a
 * thumbnail box, and audio min/max peaks for a waveform. Both functions
 * return plain owned buffers — no GTK or GDK type crosses this layer —
 * so they run identically on the worker thread and in GTK-free tests.
 *
 * GTK-free, FFmpeg-only per the adapter leak rule (oe_ffmpeg.h).
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * OE_THUMBNAIL_BOX: thumbnails fit inside this square, aspect preserved.
 */
#define OE_THUMBNAIL_BOX 96

/**
 * OE_WAVEFORM_BUCKETS: fixed number of min/max peak pairs per waveform.
 */
#define OE_WAVEFORM_BUCKETS 100

/**
 * OeThumbnail:
 * @width: scaled width in pixels (1..OE_THUMBNAIL_BOX)
 * @height: scaled height in pixels (1..OE_THUMBNAIL_BOX)
 * @rgba: width * height * 4 bytes of RGBA8 pixels, owned
 *
 * Freed with oe_thumbnail_free().
 */
typedef struct
{
  gint width;
  gint height;
  guchar *rgba;
} OeThumbnail;

/**
 * OeWaveform:
 * @bucket_count: number of peak pairs (OE_WAVEFORM_BUCKETS)
 * @peaks: 2 * bucket_count floats, min then max per bucket, each in
 *   [-1.0, 1.0], owned
 *
 * Freed with oe_waveform_free().
 */
typedef struct
{
  gint bucket_count;
  gfloat *peaks;
} OeWaveform;

void oe_thumbnail_free (OeThumbnail *thumb);
void oe_waveform_free (OeWaveform *wf);

/**
 * OeMediaJobCancel: called between decode steps.
 *
 * Returns: TRUE when the caller wants the job abandoned.
 */
typedef gboolean (*OeMediaJobCancel) (gpointer user_data);

/**
 * OE_MEDIA_JOB_ERROR: error domain for decode jobs.
 */
#define OE_MEDIA_JOB_ERROR (oe_media_job_error_quark ())

GQuark oe_media_job_error_quark (void);

/**
 * OeMediaJobError:
 * @OE_MEDIA_JOB_ERROR_OPEN_FAILED: the file could not be opened or is
 *   not a media container.
 * @OE_MEDIA_JOB_ERROR_UNSUPPORTED: no decodable stream of the required
 *   kind (e.g. a thumbnail from an audio-only file).
 * @OE_MEDIA_JOB_ERROR_CANCELLED: the cancel check stopped the job.
 */
typedef enum
{
  OE_MEDIA_JOB_ERROR_OPEN_FAILED,
  OE_MEDIA_JOB_ERROR_UNSUPPORTED,
  OE_MEDIA_JOB_ERROR_CANCELLED,
} OeMediaJobError;

/**
 * oe_media_job_thumbnail:
 * @path: media file to decode
 * @cancel: optional cancel check run between decode steps
 * @cancel_data: user data for @cancel
 * @out: thumbnail to fill on success (freed by the caller on failure)
 * @error: return location for a #GError, or NULL to ignore
 *
 * Decodes one representative video frame — seeked to 10% of the
 * duration, capped at 3 s, falling back to the first frame when the
 * seek fails — and scales it with swscale to fit the OE_THUMBNAIL_BOX
 * square, preserving aspect ratio.
 *
 * Returns: TRUE on success with @out filled.
 */
gboolean oe_media_job_thumbnail (const gchar *path, OeMediaJobCancel cancel, gpointer cancel_data,
                                 OeThumbnail *out, GError **error);

/**
 * oe_media_job_waveform:
 * @path: media file to decode
 * @cancel: optional cancel check run between decode steps
 * @cancel_data: user data for @cancel
 * @out: waveform to fill on success (freed by the caller on failure)
 * @error: return location for a #GError, or NULL to ignore
 *
 * Decodes the audio stream through swresample into mono peaks: min/max
 * pairs for OE_WAVEFORM_BUCKETS equal time buckets covering the file.
 *
 * Returns: TRUE on success with @out filled.
 */
gboolean oe_media_job_waveform (const gchar *path, OeMediaJobCancel cancel, gpointer cancel_data,
                                OeWaveform *out, GError **error);

G_END_DECLS
