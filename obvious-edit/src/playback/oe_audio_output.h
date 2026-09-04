/* oe_audio_output.h — lifecycle adapter around SDL3 audio (Phases 0 + 5).
 *
 * SDL entry points never leak past this header: callers see only the GError
 * pattern and the idempotent init/shutdown pair, plus the Phase 5 push-model
 * device stream (open once, queue interleaved f32, report depth, flush,
 * pause/resume, close).
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
 * @OE_AUDIO_OUTPUT_ERROR_NOT_INITIALIZED: the stream was opened before
 *   oe_audio_output_init() succeeded.
 * @OE_AUDIO_OUTPUT_ERROR_DEVICE_FAILED: no usable default playback device,
 *   or the device stream could not be created.
 */
typedef enum
{
  OE_AUDIO_OUTPUT_ERROR_INIT_FAILED,
  OE_AUDIO_OUTPUT_ERROR_NOT_INITIALIZED,
  OE_AUDIO_OUTPUT_ERROR_DEVICE_FAILED,
} OeAudioOutputError;

/**
 * OeAudioDeviceInfo: what a caller must know about the opened stream.
 * @sample_rate: device rate in Hz — push frames at this rate
 * @channels: device channels (interleaved)
 * @is_dummy: TRUE when the driver is SDL's dummy output — no real device
 *     consumes the queue, so the stream verifies the adapter contract but
 *     never produces sound or trustworthy pacing
 */
typedef struct
{
  int sample_rate;
  int channels;
  gboolean is_dummy;
} OeAudioDeviceInfo;

/**
 * OeAudioStream: an opaque push-model playback stream.
 *
 * Opened with oe_audio_output_open_stream(), fed with interleaved f32
 * frames at the reported device rate/channels, closed with
 * oe_audio_output_close_stream(). Main-thread-only calls, like the rest
 * of this adapter: SDL consumes the queue on its own audio thread, but
 * every function here runs on the thread that owns the session.
 */
typedef struct _OeAudioStream OeAudioStream;

/**
 * oe_audio_output_open_stream:
 * @info: receives the device rate, channel count, and dummy flag
 * @error: return location for a #GError, or NULL to ignore
 *
 * Opens a push-model stream on the default playback device requesting the
 * device's own format as 32-bit float, so SDL never resamples. The device
 * starts paused; call oe_audio_output_set_running() to unpause. Failure is
 * a typed error, never a crash — a headless or device-less machine is a
 * supported state, and callers continue without audio.
 *
 * Returns: (transfer full): the stream, or NULL with @error set.
 */
OeAudioStream *oe_audio_output_open_stream (OeAudioDeviceInfo *info, GError **error);

/**
 * oe_audio_output_queue:
 * @stream: an open stream, or NULL (a no-op returning 0)
 * @interleaved: @n_frames * device_channels floats
 * @n_frames: sample frames (one frame = @channels samples)
 *
 * Push-model queueing: frames are copied into the SDL stream and consumed
 * by the device as it runs.
 *
 * Returns: the number of frames accepted (0 on failure or NULL input).
 */
gsize oe_audio_output_queue (OeAudioStream *stream, const float *interleaved, gsize n_frames);

/**
 * oe_audio_output_queued_frames:
 * @stream: an open stream, or NULL (returns 0)
 *
 * Returns: frames queued but not yet consumed by the device.
 */
gsize oe_audio_output_queued_frames (OeAudioStream *stream);

/**
 * oe_audio_output_flush:
 * @stream: an open stream, or NULL (a no-op)
 *
 * Drops all queued audio (seek discipline: flush the device, then
 * re-anchor what plays next).
 */
void oe_audio_output_flush (OeAudioStream *stream);

/**
 * oe_audio_output_set_running:
 * @stream: an open stream, or NULL (a no-op)
 * @running: TRUE to resume the device, FALSE to pause it
 *
 * Pausing freezes device consumption; queued audio survives a
 * pause/resume round trip. Playback keeps the device running until
 * pause or teardown.
 */
void oe_audio_output_set_running (OeAudioStream *stream, gboolean running);

/**
 * oe_audio_output_close_stream:
 * @stream: (transfer full) the stream to close, or NULL (a no-op)
 *
 * Closes and frees the stream. The owner closes it BEFORE
 * oe_audio_output_shutdown(), keeping the adapter's reverse-order
 * teardown intact.
 */
void oe_audio_output_close_stream (OeAudioStream *stream);

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
