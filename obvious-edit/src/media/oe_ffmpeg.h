/* oe_ffmpeg.h — lifecycle adapter around the FFmpeg libraries (Phase 0).
 *
 * FFmpeg entry points never leak past this header: callers see only the
 * GError pattern and the idempotent init/shutdown pair.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * OE_FFMPEG_ERROR:
 *
 * Error domain for FFmpeg adapter failures.
 */
#define OE_FFMPEG_ERROR (oe_ffmpeg_error_quark ())

GQuark oe_ffmpeg_error_quark (void);

/**
 * OeFfmpegError:
 * @OE_FFMPEG_ERROR_INIT_FAILED: FFmpeg library initialisation failed.
 */
typedef enum
{
  OE_FFMPEG_ERROR_INIT_FAILED,
} OeFfmpegError;

/**
 * oe_ffmpeg_init:
 * @error: return location for a #GError, or NULL to ignore
 *
 * Initialises the FFmpeg libraries the project links (currently network
 * support in libavformat) and logs each linked library version through the
 * OE logging domain. Idempotent: calling it again after success is a no-op
 * that still returns TRUE.
 *
 * Returns: TRUE on success; on failure FALSE with @error set (caller frees).
 */
gboolean oe_ffmpeg_init (GError **error);

/**
 * oe_ffmpeg_shutdown:
 *
 * Paired cleanup for oe_ffmpeg_init(). Safe to call twice and safe to call
 * before init; in both cases it does nothing.
 */
void oe_ffmpeg_shutdown (void);

/**
 * oe_ffmpeg_is_initialized:
 *
 * Returns: TRUE after a successful oe_ffmpeg_init() and before the matching
 * oe_ffmpeg_shutdown().
 */
gboolean oe_ffmpeg_is_initialized (void);

G_END_DECLS
