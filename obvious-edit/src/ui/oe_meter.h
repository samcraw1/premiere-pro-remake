/* oe_meter.h — the audio peak meter widget (Phase 10 Wave B, D6).
 *
 * A GtkDrawingArea that draws one bar per audio channel from the
 * per-chunk peaks the playback session delivers on the main context.
 * The pure rules it draws by (per-update decay, bar geometry) live
 * GTK-free in oe_meter_math.h so the audio-tools suite can verify
 * them without a display.
 *
 * No timers: during playback every chunk refreshes (and decays) the
 * bars; pausing or scrubbing stops chunk flow, and the session's
 * release() write zeroes the levels so the meter settles at silence.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define OE_TYPE_METER (oe_meter_get_type ())
G_DECLARE_FINAL_TYPE (OeMeter, oe_meter, OE, METER, GtkDrawingArea)

/** oe_meter_new:
 *
 * Returns: (transfer full): a new peak meter.
 */
OeMeter *oe_meter_new (void);

/** oe_meter_set_peaks:
 * @peaks: per-channel linear peaks in [0, 1]
 *
 * Applies the decay rule per channel (oe_meter_decay_level), stores
 * the result, and repaints. Main-context only — chunk delivery is.
 */
void oe_meter_set_peaks (OeMeter *meter, const gfloat *peaks, guint n_channels);

/** oe_meter_release:
 *
 * zeroes every level: the pause/scrub settle (no chunks flow, so the
 * meter must not freeze mid-scale).
 */
void oe_meter_release (OeMeter *meter);

G_END_DECLS
