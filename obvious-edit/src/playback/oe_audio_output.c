#include "oe_audio_output.h"

#include "../app/oe_log.h"

#include <SDL3/SDL.h>

/*
 * Same main-thread-only discipline as the FFmpeg adapter; see oe_ffmpeg.c.
 */
static gboolean audio_output_initialized = FALSE;

GQuark
oe_audio_output_error_quark (void)
{
  return g_quark_from_static_string ("oe-audio-output-error");
}

gboolean
oe_audio_output_init (GError **error)
{
  int version;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (audio_output_initialized)
    {
      oe_log (OE_LOG_LEVEL_DEBUG, "audio output adapter already initialised");
      return TRUE;
    }

  if (!SDL_Init (SDL_INIT_AUDIO))
    {
      g_set_error (error, OE_AUDIO_OUTPUT_ERROR, OE_AUDIO_OUTPUT_ERROR_INIT_FAILED,
                   "SDL_Init (SDL_INIT_AUDIO) failed: %s", SDL_GetError ());
      return FALSE;
    }

  audio_output_initialized = TRUE;

  version = SDL_GetVersion ();
  oe_log (OE_LOG_LEVEL_INFO, "SDL3 %d.%d.%d linked, audio subsystem ready",
          SDL_VERSIONNUM_MAJOR (version), SDL_VERSIONNUM_MINOR (version),
          SDL_VERSIONNUM_MICRO (version));
  oe_log (OE_LOG_LEVEL_DEBUG, "audio output adapter initialised");
  return TRUE;
}

void
oe_audio_output_shutdown (void)
{
  if (!audio_output_initialized)
    return;

  SDL_Quit ();
  audio_output_initialized = FALSE;
  oe_log (OE_LOG_LEVEL_DEBUG, "audio output adapter shut down");
}

gboolean
oe_audio_output_is_initialized (void)
{
  return audio_output_initialized;
}
