/* oe_audio_buffer.h — GTK-free buffer math over the shared interleaved
 * f32 frame format (Phase 10 Wave B). One seam for the per-channel peak
 * extraction the playback session and the export mixdown both need, so
 * metering and parity tests cannot grow a second definition of "peak".
 *
 * No GTK types, no clocks, no side effects: total functions over plain
 * buffers. */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * oe_audio_buffer_peak:
 * @interleaved: n_frames * channels floats (may be NULL — reads as 0)
 * @n_frames: sample frames in the span (one frame = @channels samples)
 * @channels: interleaved channel count (clamped to >= 1)
 * @channel: which channel to scan (clamped into range)
 *
 * Returns: the maximum absolute sample value of @channel across the
 * span — the per-channel peak a meter displays for the span. A NULL or
 * empty span peaks at 0.
 */
gfloat oe_audio_buffer_peak (const gfloat *interleaved, gsize n_frames, int channels,
                             int channel);

G_END_DECLS
