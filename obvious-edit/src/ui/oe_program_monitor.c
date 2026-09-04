/* oe_program_monitor.c — the program-monitor drawing area (Phase 5).
 *
 * Deep-copy draw discipline: the widget owns its pixel buffer
 * exclusively. The session hands over an OePlaybackVideoFrame; the
 * monitor wraps it in a cairo surface at draw time without copying —
 * the surface wrapper borrows the buffer for the duration of the paint
 * only. Nothing else holds a reference between deliveries.
 */

#include "oe_program_monitor.h"

#include "../app/oe_playback_session.h"

struct _OeProgramMonitor
{
  GtkDrawingArea parent_instance;

  OePlaybackVideoFrame *frame; /* owned between deliveries */
  gboolean missing;
};

G_DEFINE_TYPE (OeProgramMonitor, oe_program_monitor, GTK_TYPE_DRAWING_AREA)

/* Palette follows the shell's dark interface (see the timeline widget). */
#define MONITOR_BG_R 0.090
#define MONITOR_BG_G 0.090
#define MONITOR_BG_B 0.102

static void
draw_text_centered (cairo_t *cr, double cx, double cy, const char *text)
{
  cairo_text_extents_t extents;

  cairo_select_font_face (cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, 12);
  cairo_text_extents (cr, text, &extents);

  cairo_move_to (cr, cx - extents.width / 2, cy);
  cairo_set_source_rgb (cr, 0.604, 0.604, 0.635);
  cairo_show_text (cr, text);
}

/* Missing-media hatch: diagonal strokes over the whole monitor. */
static void
draw_hatch (cairo_t *cr, int width, int height)
{
  const double step = 14.0;

  cairo_save (cr);
  cairo_rectangle (cr, 0, 0, width, height);
  cairo_clip (cr);

  cairo_set_source_rgba (cr, 0.862, 0.313, 0.313, 0.45);
  cairo_set_line_width (cr, 1.0);

  for (double x = -height; x < width; x += step)
    {
      cairo_move_to (cr, x, 0);
      cairo_line_to (cr, x + height, height);
    }

  cairo_stroke (cr);
  cairo_restore (cr);
}

/* Box-fit the frame into the widget: center it, scale down if larger,
 * never upscale (the session already delivers a monitor-sized frame). */
static void
draw_frame (cairo_t *cr, OeProgramMonitor *self, int width, int height)
{
  const OePlaybackVideoFrame *frame = self->frame;
  cairo_surface_t *surface;

  if (frame->width <= 0 || frame->height <= 0 || frame->rgba == NULL)
    return;

  surface = cairo_image_surface_create_for_data (frame->rgba, CAIRO_FORMAT_ARGB32, frame->width,
                                                 frame->height, frame->width * 4);

  const double scale = MIN ((double) width / frame->width, (double) height / frame->height);
  const double draw_w = frame->width * scale;
  const double draw_h = frame->height * scale;

  cairo_save (cr);
  cairo_translate (cr, (width - draw_w) / 2.0, (height - draw_h) / 2.0);

  if (scale < 1.0)
    cairo_scale (cr, scale, scale);

  cairo_set_source_surface (cr, surface, 0, 0);
  cairo_paint (cr);
  cairo_restore (cr);

  cairo_surface_destroy (surface);
}

static void
on_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
  OeProgramMonitor *self = OE_PROGRAM_MONITOR (area);

  (void) user_data;

  cairo_set_source_rgb (cr, MONITOR_BG_R, MONITOR_BG_G, MONITOR_BG_B);
  cairo_paint (cr);

  if (self->missing)
    {
      draw_hatch (cr, width, height);
      draw_text_centered (cr, width / 2.0, height / 2.0, "Missing media — playback continues");
      return;
    }

  if (self->frame != NULL)
    {
      draw_frame (cr, self, width, height);
      return;
    }

  /* Empty state: kept until the first frame of a session. */
  draw_text_centered (cr, width / 2.0, height / 2.0, "No program output yet");
  draw_text_centered (cr, width / 2.0, height / 2.0 + 18.0, "Press Space to play");
}

static void
oe_program_monitor_dispose (GObject *object)
{
  OeProgramMonitor *self = OE_PROGRAM_MONITOR (object);

  g_clear_pointer (&self->frame, oe_playback_video_frame_free);

  G_OBJECT_CLASS (oe_program_monitor_parent_class)->dispose (object);
}

static void
oe_program_monitor_class_init (OeProgramMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = oe_program_monitor_dispose;
}

static void
oe_program_monitor_init (OeProgramMonitor *self)
{
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self), on_draw, NULL, NULL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
}

OeProgramMonitor *
oe_program_monitor_new (void)
{
  return g_object_new (OE_TYPE_PROGRAM_MONITOR, NULL);
}

void
oe_program_monitor_show_frame (OeProgramMonitor *monitor, OePlaybackVideoFrame *frame)
{
  g_return_if_fail (OE_IS_PROGRAM_MONITOR (monitor));

  monitor->missing = FALSE;
  g_clear_pointer (&monitor->frame, oe_playback_video_frame_free);
  monitor->frame = frame; /* adopt */

  gtk_widget_queue_draw (GTK_WIDGET (monitor));
}

void
oe_program_monitor_set_missing (OeProgramMonitor *monitor, gboolean missing)
{
  g_return_if_fail (OE_IS_PROGRAM_MONITOR (monitor));

  if (monitor->missing == missing)
    return;

  monitor->missing = missing;
  gtk_widget_queue_draw (GTK_WIDGET (monitor));
}

void
oe_program_monitor_clear (OeProgramMonitor *monitor)
{
  g_return_if_fail (OE_IS_PROGRAM_MONITOR (monitor));

  g_clear_pointer (&monitor->frame, oe_playback_video_frame_free);
  monitor->missing = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (monitor));
}
