/* oe_export.h — GTK-free synchronous MP4 export job (Phase 8).
 *
 * Renders a sequence snapshot to an H.264/AAC MP4 file:
 *
 *   - video: the frame-at-time seam (oe_render.h) sampled on the
 *     integer frame grid — frame f renders at
 *     oe_time_frame_to_us(f, rate); total frames = ceil(sequence end
 *     over the frame interval). No per-frame seeks: one decoder per
 *     source path reads forward.
 *   - audio: additive mixdown of every audio track in array order,
 *     gaps silent, 48 kHz stereo, float sum with a hard ±1.0 clamp.
 *   - encode: H.264 through libx264 by name (any installed H.264
 *     encoder by id as fallback), yuv420p, x264 preset veryfast, CRF
 *     from the quality preset; native AAC at 48 kHz stereo; MP4
 *     muxer.
 *   - finalize mirrors oe_project_format's write_atomic: a g_mkstemp
 *     temp file in the TARGET directory, avformat_write_header →
 *     frame pump → av_write_trailer → fsync of the temp fd →
 *     g_rename over the destination — the destination is touched only
 *     on full success; the temp file is unlinked on every failure,
 *     cancellation included.
 *
 * Calling convention matches the media jobs (oe_media_jobs.h):
 * callbacks, a gboolean-returning cancel function consulted between
 * frames, and a typed GError domain. The job is fully synchronous and
 * GTK-free — the UI owns any thread around it — and oe_export_run()
 * is directly drivable from tests with no dialog in sight.
 */

#pragma once

#include <glib.h>

#include "../core/oe_project.h"
#include "../core/oe_time.h"

G_BEGIN_DECLS

/**
 * OE_EXPORT_ERROR: error domain for export jobs.
 */
#define OE_EXPORT_ERROR (oe_export_error_quark ())

GQuark oe_export_error_quark (void);

/**
 * OeExportError:
 * @OE_EXPORT_ERROR_FAILED: rendering, decoding, or muxing failed.
 * @OE_EXPORT_ERROR_OPEN_FAILED: the destination's directory is not
 *     writable or the temp file could not be created.
 * @OE_EXPORT_ERROR_CANCELLED: the cancel function returned TRUE —
 *     the destination was not touched.
 * @OE_EXPORT_ERROR_ENCODER: no usable H.264 or AAC encoder exists.
 */
typedef enum
{
  OE_EXPORT_ERROR_FAILED,
  OE_EXPORT_ERROR_OPEN_FAILED,
  OE_EXPORT_ERROR_CANCELLED,
  OE_EXPORT_ERROR_ENCODER,
} OeExportError;

/**
 * OeExportQuality: video quality presets mapped to x264 CRF values.
 * @OE_EXPORT_QUALITY_HIGH: CRF 18.
 * @OE_EXPORT_QUALITY_MEDIUM: CRF 23 (the default choice).
 * @OE_EXPORT_QUALITY_LOW: CRF 28.
 */
typedef enum
{
  OE_EXPORT_QUALITY_HIGH,
  OE_EXPORT_QUALITY_MEDIUM,
  OE_EXPORT_QUALITY_LOW,
} OeExportQuality;

/**
 * OeExportResolveFunc: maps a media reference to its file path.
 * Returns NULL when the reference is unresolvable (missing media).
 * Follows the render seam's resolver contract — the export job runs
 * away from the main thread, so callers hand it a stable snapshot or
 * copy, never the live library.
 *
 * Returns: (transfer full): an owned path, or NULL.
 */
typedef gchar *(*OeExportResolveFunc) (guint media_ref, gpointer user_data);

/**
 * OeExportSpec: everything one export run needs. All pointers are
 * borrowed for the duration of oe_export_run() — the caller keeps
 * them alive and immutable while the job runs (a deep-copied sequence
 * snapshot is the UI's responsibility).
 */
typedef struct
{
  const OeSequence *sequence;
  OeExportResolveFunc resolve_path;
  gpointer resolve_data;
  const gchar *destination_path;
  OeExportQuality quality;
} OeExportSpec;

/**
 * OeExportCancelFunc: return TRUE to cancel the export. Consulted
 * between frames; the job then fails with
 * #OE_EXPORT_ERROR_CANCELLED and leaves the destination untouched.
 */
typedef gboolean (*OeExportCancelFunc) (gpointer user_data);

/**
 * OeExportProgressFunc: called after each finished video frame.
 * @frame_index counts finished frames (1-based after the first),
 * @total_frames is the grid total.
 */
typedef void (*OeExportProgressFunc) (gint64 frame_index, gint64 total_frames, gpointer user_data);

/**
 * oe_export_frame_count:
 * @sequence: the sequence to export
 *
 * The frame grid: total frames = ceil(sequence_end_us / frame
 * interval), computed in integers from the sequence's frame rate.
 * Zero for an empty sequence.
 *
 * Returns: the total number of frames in the export grid.
 */
gint64 oe_export_frame_count (const OeSequence *sequence);

/**
 * oe_export_frame_time_us:
 * @frame_index: zero-based frame position
 * @sequence: the sequence to export
 *
 * The grid discipline: frame f renders at
 * oe_time_frame_to_us(f, rate) — the same conversion the timeline
 * playhead uses.
 *
 * Returns: the frame's sequence time in µs.
 */
gint64 oe_export_frame_time_us (gint64 frame_index, const OeSequence *sequence);

/**
 * oe_export_run:
 * @spec: the export specification (borrowed for the whole call)
 * @cancel_fn: cancel callback, or NULL to never cancel
 * @cancel_data: user data for @cancel_fn
 * @progress_fn: progress callback, or NULL to ignore progress
 * @progress_data: user data for @progress_fn
 * @error: return location for a #GError, or NULL to ignore
 *
 * Runs the export synchronously on the calling thread. On success the
 * destination file exists and is complete; on any failure — including
 * cancellation — the destination is byte-identical to before the call
 * and no temp file survives.
 *
 * Returns: TRUE on success.
 */
gboolean oe_export_run (const OeExportSpec *spec, OeExportCancelFunc cancel_fn,
                        gpointer cancel_data, OeExportProgressFunc progress_fn,
                        gpointer progress_data, GError **error);

G_END_DECLS
