/* oe_meter_math.c — the pure meter rules (Phase 10 Wave B, D6).
 *
 * Decay is per UPDATE, not wall time: meters settle by stopping at
 * the last held level when no chunks flow (the widget's release
 * write handles the pause/scrub settle). Geometry is integer math so
 * GTK-free tests can pin it exactly. No GTK here by design.
 */

#include "oe_meter_math.h"

/* Documented per-update fall in linear [0,1] units (about -16% per
 * chunk at 48 kHz with ~20 ms chunks — visible fall, no flicker). */
#define METER_DECAY_STEP 0.04f

#define METER_GAP_PX 4
#define METER_MIN_WIDTH 8

gfloat
oe_meter_decay_step (void)
{
  return METER_DECAY_STEP;
}

gfloat
oe_meter_decay_level (gfloat displayed, gfloat incoming)
{
  gfloat held = displayed - METER_DECAY_STEP;

  if (incoming > held)
    held = incoming; /* peak rises instantly, falls per update */

  if (held < 0.0f)
    held = 0.0f;

  if (held > 1.0f)
    held = 1.0f;

  return held;
}

int
oe_meter_bar_geometry (int width, int height, guint n_bars, guint index, int *bar_x)
{
  int bar = 0;

  if (height < 0)
    height = 0;

  if (n_bars > 0)
    {
      const int total = (int) n_bars * METER_MIN_WIDTH + (int) (n_bars - 1) * METER_GAP_PX;

      if (width >= total)
        bar = (width - (int) (n_bars - 1) * METER_GAP_PX) / (int) n_bars;
    }

  if (index >= n_bars || bar <= 0)
    return bar; /* nothing drawable: no origin is written either */

  if (bar_x != NULL)
    {
      const int span = n_bars * bar + (n_bars - 1) * METER_GAP_PX;

      *bar_x = (width - span) / 2 + (int) index * (bar + METER_GAP_PX);
    }

  return bar;
}
