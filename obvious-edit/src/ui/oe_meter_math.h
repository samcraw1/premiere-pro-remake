/* oe_meter_math.h — GTK-free meter math (Phase 10 Wave B, D6).
 *
 * The pure helpers behind the audio peak meter: the per-update decay
 * rule and the Cairo bar layout. No GTK types, no clocks, no side
 * effects — the audio-tools suite verifies them without a display,
 * and the widget (oe_meter.c) is a thin shell over them.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/** oe_meter_decay_step:
 *
 * The documented per-update fall (linear [0,1] units).
 */
gfloat oe_meter_decay_step (void);

/** oe_meter_decay_level:
 *
 * Pure decay rule: the displayed level falls by OE_METER_DECAY_STEP
 * per update toward the incoming peak, never rising past it, and
 * clamps to [0, 1]. Falling beats snapping — a peak between chunks
 * holds instead of flickering.
 */
gfloat oe_meter_decay_level (gfloat displayed, gfloat incoming);

/** oe_meter_bar_geometry:
 *
 * Pure bar layout for the Cairo paint: @n_bars equal-width bars with
 * fixed 4 px gaps, centered in @width; every bar spans the full
 * @height. Returns the bar width (>= 0), or 0 when @width cannot fit
 * @n_bars bars of at least 8 px each — the layout never draws
 * unreadable slivers; writes the x origin of bar @index through
 * @bar_x when non-NULL. Geometry is integer math so GTK-free tests
 * can pin it exactly.
 */
int oe_meter_bar_geometry (int width, int height, guint n_bars, guint index, int *bar_x);

G_END_DECLS
