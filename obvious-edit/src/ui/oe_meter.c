/* oe_meter.c — the audio peak meter (Phase 10 Wave B, D6).
 *
 * One Cairo bar per channel, drawn from the stored levels. The decay
 * and geometry rules are the GTK-free pure helpers in oe_meter_math.h;
 * this file only wires them to widget state and paint. No timers:
 * repaints are notify-driven exactly like the program monitor.
 */

#include "oe_meter.h"

#include "oe_meter_math.h"

#define METER_BG_R 0.110
#define METER_BG_G 0.110
#define METER_BG_B 0.118
#define METER_BAR_R 0.322
#define METER_BAR_G 0.714
#define METER_BAR_B 0.427
#define METER_TRACK_R 0.208
#define METER_TRACK_G 0.208
#define METER_TRACK_B 0.224

struct _OeMeter
{
  GtkDrawingArea parent_instance;

  gfloat levels[2]; /* linear [0,1], one per drawn bar */
  guint n_levels;
};

G_DEFINE_TYPE (OeMeter, oe_meter, GTK_TYPE_DRAWING_AREA)

static void
on_meter_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
  OeMeter *meter = OE_METER (area);

  (void) user_data;

  cairo_set_source_rgb (cr, METER_BG_R, METER_BG_G, METER_BG_B);
  cairo_paint (cr);

  for (guint ch = 0; ch < meter->n_levels; ch++)
    {
      int x = 0;

      const int bar = oe_meter_bar_geometry (width, height, meter->n_levels, ch, &x);

      if (bar <= 0)
        continue;

      /* The channel track behind the level bar. */
      cairo_set_source_rgb (cr, METER_TRACK_R, METER_TRACK_G, METER_TRACK_B);
      cairo_rectangle (cr, x, 0, bar, height);
      cairo_fill (cr);

      /* The level itself, anchored at the bottom. */
      const gfloat level = CLAMP (meter->levels[ch], 0.0f, 1.0f);

      if (level > 0.0f)
        {
          cairo_set_source_rgb (cr, METER_BAR_R, METER_BAR_G, METER_BAR_B);
          cairo_rectangle (cr, x, height - (int) (level * (gfloat) height), bar,
                           (int) (level * (gfloat) height));
          cairo_fill (cr);
        }
    }
}

void
oe_meter_set_peaks (OeMeter *meter, const gfloat *peaks, guint n_channels)
{
  g_return_if_fail (OE_IS_METER (meter));

  const guint n = MIN (n_channels, G_N_ELEMENTS (meter->levels));

  for (guint ch = 0; ch < n; ch++)
    meter->levels[ch] = oe_meter_decay_level (meter->levels[ch], peaks[ch]);

  meter->n_levels = n;
  gtk_widget_queue_draw (GTK_WIDGET (meter));
}

void
oe_meter_release (OeMeter *meter)
{
  g_return_if_fail (OE_IS_METER (meter));

  meter->levels[0] = 0.0f;
  meter->levels[1] = 0.0f;
  meter->n_levels = 0;
  gtk_widget_queue_draw (GTK_WIDGET (meter));
}

static void
oe_meter_init (OeMeter *self)
{
  /* A slim strip above the program monitor: full width, fixed height. */
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self), on_meter_draw, NULL, NULL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), FALSE);
  /* Two bars at the 8 px layout minimum plus a gap: the smallest
   * strip that can ever show stereo. */
  gtk_widget_set_size_request (GTK_WIDGET (self), 16 + 4, 40);
}

/* Nothing to install beyond the defaults: drawing rides the draw
 * func, and the widget exposes no properties or signals. GObject
 * still requires the class-init symbol via G_DEFINE_TYPE. */
static void
oe_meter_class_init (OeMeterClass *klass G_GNUC_UNUSED)
{
}

OeMeter *
oe_meter_new (void)
{
  return g_object_new (OE_TYPE_METER, NULL);
}
