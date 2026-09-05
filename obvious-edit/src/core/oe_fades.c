/* Audio fade envelope — implementation of src/core/oe_fades.h. */

#include "oe_fades.h"

#include "oe_time.h"

guint
oe_fade_gain (gint64 t_us, gint64 clip_start_us, gint64 clip_end_us, guint64 fade_in_us,
              guint64 fade_out_us)
{
  g_return_val_if_fail (clip_end_us > clip_start_us, OE_FADE_SCALE);

  const gint64 into_us = t_us - clip_start_us;
  const gint64 left_us = clip_end_us - t_us;

  /* Outside the window there is no envelope to speak of; callers only
   * ask for in-window samples, so treat the closed edges as full gain
   * rather than guessing a ramp direction. */
  if (into_us < 0 || left_us < 0)
    return OE_FADE_SCALE;

  /* Each fade side is independent: a zero-length side is disabled
   * (its raw ratio would divide by zero) and must not attenuate the
   * other side — a clip with only a fade-out still fades out. */
  gint64 gain = OE_FADE_SCALE;

  if (fade_in_us > 0)
    gain = MIN (gain, oe_time_round_ratio (into_us * (gint64) OE_FADE_SCALE, (gint64) fade_in_us));

  if (fade_out_us > 0)
    gain = MIN (gain, oe_time_round_ratio (left_us * (gint64) OE_FADE_SCALE, (gint64) fade_out_us));

  return (guint) CLAMP (gain, 0, OE_FADE_SCALE);
}
