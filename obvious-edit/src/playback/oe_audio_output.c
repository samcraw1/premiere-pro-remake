#include "oe_audio_output.h"

#include "../app/oe_log.h"

#include <SDL3/SDL.h>

/*
 * Same main-thread-only discipline as the FFmpeg adapter; see oe_ffmpeg.c.
 * SDL consumes queued audio on its own device thread, but every function
 * here runs on the thread that owns the session.
 */
static gboolean audio_output_initialized = FALSE;

struct _OeAudioStream
{
  SDL_AudioStream *sdl_stream;
  SDL_AudioDeviceID device;
  int channels;
  int sample_rate;
  gboolean is_dummy;
};

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

OeAudioStream *
oe_audio_output_open_stream (OeAudioDeviceInfo *info, GError **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail (info != NULL, NULL);

  if (!audio_output_initialized)
    {
      g_set_error (error, OE_AUDIO_OUTPUT_ERROR, OE_AUDIO_OUTPUT_ERROR_NOT_INITIALIZED,
                   "audio subsystem not initialised — call oe_audio_output_init first");
      return NULL;
    }

  SDL_AudioSpec device_spec = { 0 };
  int sample_frames = 0;
  if (!SDL_GetAudioDeviceFormat (SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device_spec, &sample_frames))
    {
      g_set_error (error, OE_AUDIO_OUTPUT_ERROR, OE_AUDIO_OUTPUT_ERROR_DEVICE_FAILED,
                   "no default playback device: %s", SDL_GetError ());
      return NULL;
    }

  /* Request the device's own rate and channel count as 32-bit float so the
   * stream never resamples: what the session pushes is what plays. */
  SDL_AudioSpec want = { SDL_AUDIO_F32, device_spec.channels, device_spec.freq };
  SDL_AudioStream *sdl_stream
      = SDL_OpenAudioDeviceStream (SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NULL, NULL);
  if (sdl_stream == NULL)
    {
      g_set_error (error, OE_AUDIO_OUTPUT_ERROR, OE_AUDIO_OUTPUT_ERROR_DEVICE_FAILED,
                   "opening the device stream failed: %s", SDL_GetError ());
      return NULL;
    }

  OeAudioStream *stream = g_new0 (OeAudioStream, 1);
  stream->sdl_stream = sdl_stream;
  stream->device = SDL_GetAudioStreamDevice (sdl_stream);
  stream->channels = device_spec.channels;
  stream->sample_rate = device_spec.freq;
  stream->is_dummy = g_strcmp0 (SDL_GetCurrentAudioDriver (), "dummy") == 0;

  /* Deterministic contract: the device stays paused until the first
   * oe_audio_output_set_running (TRUE). */
  SDL_PauseAudioDevice (stream->device);

  info->sample_rate = stream->sample_rate;
  info->channels = stream->channels;
  info->is_dummy = stream->is_dummy;

  oe_log (OE_LOG_LEVEL_INFO, "audio device open: %d Hz, %d ch, driver '%s'%s", stream->sample_rate,
          stream->channels, SDL_GetCurrentAudioDriver (),
          stream->is_dummy ? " (dummy — no audible output)" : "");
  return stream;
}

gsize
oe_audio_output_queue (OeAudioStream *stream, const float *interleaved, gsize n_frames)
{
  if (stream == NULL || interleaved == NULL || n_frames == 0)
    return 0;

  gsize bytes = n_frames * (gsize) stream->channels * sizeof (float);
  if (!SDL_PutAudioStreamData (stream->sdl_stream, interleaved, (int) bytes))
    {
      oe_log (OE_LOG_LEVEL_WARNING, "audio queue push failed: %s", SDL_GetError ());
      return 0;
    }
  return n_frames;
}

gsize
oe_audio_output_queued_frames (OeAudioStream *stream)
{
  if (stream == NULL)
    return 0;

  int bytes = SDL_GetAudioStreamQueued (stream->sdl_stream);
  if (bytes < 0)
    return 0;

  gsize frame_bytes = (gsize) stream->channels * sizeof (float);
  return frame_bytes > 0 ? (gsize) bytes / frame_bytes : 0;
}

void
oe_audio_output_flush (OeAudioStream *stream)
{
  if (stream == NULL)
    return;

  if (SDL_ClearAudioStream (stream->sdl_stream))
    oe_log (OE_LOG_LEVEL_DEBUG, "audio stream flushed (seek)");
  else
    oe_log (OE_LOG_LEVEL_WARNING, "audio stream flush failed: %s", SDL_GetError ());
}

void
oe_audio_output_set_running (OeAudioStream *stream, gboolean running)
{
  if (stream == NULL)
    return;

  if (running)
    SDL_ResumeAudioDevice (stream->device);
  else
    SDL_PauseAudioDevice (stream->device);
}

void
oe_audio_output_close_stream (OeAudioStream *stream)
{
  if (stream == NULL)
    return;

  SDL_DestroyAudioStream (stream->sdl_stream);
  oe_log (OE_LOG_LEVEL_INFO, "audio device closed");
  g_free (stream);
}
