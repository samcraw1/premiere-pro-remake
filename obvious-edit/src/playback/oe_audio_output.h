/* oe_audio_output.h — lifecycle adapter around SDL3 audio (Phase 0).
 *
 * SDL entry points never leak past this header: callers see only the GError
 * pattern and the idempotent init/shutdown pair.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * OE_AUDIO_OUTPUT_ERROR:
 *
 * Error domain for audio output adapter failures.
 */
#define OE_AUDIO_OUTPUT_ERROR (oe_audio_output_error_quark ())

GQuark oe_audio_output_error_quark (void);

/**
 * OeAudioOutputError:
 * @OE_AUDIO_OUTPUT_ERROR_INIT_FAILED: SDL audio subsystem initialisation
 *   failed.
 */
typedef enum
{
  OE_AUDIO_OUTPUT_ERROR_INIT_FAILED,
} OeAudioOutputError;

/**
 * oe_audio_output_init:
 * @error: return location for a #GError, or NULL to ignore
 *
 * Initialises the SDL3 audio subsystem (SDL_Init with SDL_INIT_AUDIO) and
 * logs the linked SDL version through the OE logging domain. Idempotent:
 * calling it again after success is a no-op that still returns TRUE.
 *
 * Returns: TRUE on success; on failure FALSE with @error set (caller frees).
 */
gboolean oe_audio_output_init (GError **error);

/**
 * oe_audio_output_shutdown:
 *
 * Paired cleanup for oe_audio_output_init(). Safe to call twice and safe to
 * call before init; in both cases it does nothing.
 */
void oe_audio_output_shutdown (void);

/**
 * oe_audio_output_is_initialized:
 *
 * Returns: TRUE after a successful oe_audio_output_init() and before the
 * matching oe_audio_output_shutdown().
 */
gboolean oe_audio_output_is_initialized (void);

G_END_DECLS
