/* oe_audio_factor.c — the shared integer audio factor chain (Phase 10
 * Wave A). See the header for the law, the scale convention, and the
 * mute/solo contract; this file is the arithmetic. */

#include "oe_audio_factor.h"

void
oe_audio_factor (gint32 fade, gint32 clip_gain, gint32 clip_pan, gint32 track_volume,
                 gint32 track_pan, gint32 mute_or_solo_zero, gint32 factor_out[2])
{
  g_return_if_fail (factor_out != NULL);

  /* D5: mute or lose-solo zeroes the whole chain — both channels, no
   * partial attenuation. */
  if (mute_or_solo_zero == 0)
    {
      factor_out[0] = 0;
      factor_out[1] = 0;
      return;
    }

  /* The linear pan pair (D3) on the chain's scale: center is unity,
   * a hard pan puts twice unity on one channel and zero on the other,
   * and the L + R sum is constant at every position. */
  const gint64 pan_pair[2] = { 2 * (OE_AUDIO_UNITY - (gint64) clip_pan),
                               2 * (gint64) clip_pan };
  const gint64 track_pair[2] = { 2 * (OE_AUDIO_UNITY - (gint64) track_pan),
                                 2 * (gint64) track_pan };

  /* Exactly one rounding, half away from zero (every stage is
   * non-negative in its domain): the product of the five 1024-scale
   * stages divides by unity^4 to land back on the 1024 scale. The
   * widest legal product is 1024 × 2048 × 2048 × 2048 × 2048 = 2^54,
   * far inside gint64 — no intermediate precision loss. */
  const gint64 den = (gint64) OE_AUDIO_UNITY * OE_AUDIO_UNITY * OE_AUDIO_UNITY * OE_AUDIO_UNITY;
  const gint64 half = den / 2;

  for (gint ch = 0; ch < 2; ch++)
    {
      const gint64 product = (gint64) fade * clip_gain * pan_pair[ch] * track_volume
                             * track_pair[ch];

      factor_out[ch] = (gint32) ((product + half) / den);
    }
}

gboolean
oe_audio_audible (gint32 mute, gint32 solo, gboolean any_solo)
{
  if (any_solo)
    return solo != 0;

  return mute == 0;
}
