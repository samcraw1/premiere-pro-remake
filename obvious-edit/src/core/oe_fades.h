/* Audio fade envelope — one shared integer gain ramp, GTK-free and
 * FFmpeg-free. Both audio consumers (the export mixdown sum loop and
 * the playback chunk path) call this helper so preview and export can
 * never drift apart.
 *
 * The envelope is a linear ramp on a fixed 0..1024 integer scale:
 *
 *   g(t) = MIN(1024, round_ratio((t - start) * 1024, fade_in),
 *                    round_ratio((end - t) * 1024, fade_out))
 *
 * with rounding through oe_time_round_ratio — exactly one rounding per
 * comparison, no intermediate precision loss. A zero-length fade side
 * contributes no attenuation (division guard yields full gain), so a
 * clip with no fades stays bit-identical to the unscaled mix.
 *
 * Consumers scale samples by g / 1024. In an integer-sample domain
 * that multiply rounds half-up through the integer contribution
 * (sample * g + 512) >> 10; this project's audio consumers are float,
 * where the rounding grid does not exist and the contribution is the
 * pure scale sample * (float) g / 1024.0f. The envelope multiplies
 * samples before the mix sum, and the downstream hard clamp is
 * unchanged. */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Fixed-point envelope scale: full gain is 2^10 (0..1024). */
#define OE_FADE_SCALE 1024

/* Gain (0..1024) for a sample at sequence time @t_us inside the clip
 * window [@clip_start_us, @clip_end_us) with fade-in @fade_in_us and
 * fade-out @fade_out_us (both in µs, 0 = disabled). Pure function of
 * its arguments: no state, no allocation. */
guint oe_fade_gain (gint64 t_us, gint64 clip_start_us, gint64 clip_end_us, guint64 fade_in_us,
                    guint64 fade_out_us);

G_END_DECLS
