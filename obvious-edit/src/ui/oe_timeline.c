/* oe_timeline.c — timeline widget implementation (Phase 4).
 *
 * One drag state machine drives every interaction, shared by the
 * gesture controllers:
 *
 *   press    → hit-test arms MOVE | TRIM_IN | TRIM_OUT | PLAYHEAD
 *              (or NOTHING: a click clears the selection)
 *   motion   → preview the CLAMPED result from the GTK-free layout
 *              core; the model is never written during a drag
 *   release  → < 4 px counts as a click (selection/playhead only);
 *              otherwise commit through the model mutators — a typed
 *              rejection is reported and the preview snaps back
 *
 * Every repaint works on a fresh get_sequence() deep copy delivered by
 * the project observer; the widget holds no model references of its
 * own. Playhead and zoom are widget-session state (Phase 5 owns the
 * clock and any persistence).
 */

#include "oe_timeline.h"

#include <math.h>

#include "oe_timeline_layout.h"

/* Zoom is session state: 100 px per second default, 1 px/s .. 1000 px/s. */
#define OE_TIMELINE_ZOOM_DEFAULT 0.0001
#define OE_TIMELINE_ZOOM_MIN 0.000001
#define OE_TIMELINE_ZOOM_MAX 0.001

/* Left margin between the widget edge and timeline position 0. */
#define OE_TIMELINE_LEFT_MARGIN 8.0

/* Movement below this many pixels is a click, not a drag. */
#define OE_TIMELINE_CLICK_THRESHOLD 4.0

typedef enum
{
  DRAG_NONE,
  DRAG_MOVE,
  DRAG_TRIM_IN,
  DRAG_TRIM_OUT,
  DRAG_PLAYHEAD,
} OeTimelineDragKind;

typedef struct
{
  OeTimelineDragKind kind;
  guint track_index;
  guint clip_index;

  /* Grab geometry, captured at press. */
  gdouble press_x;
  gdouble press_y;
  gint64 grab_offset_us; /* pointer position inside the clip, in µs */

  /* Live preview (drag only): the clamped candidate for MOVE, and the
   * clamped source edge for trims. Snap-back = reset to NONE. */
  gint64 preview_position_us;
  gint64 preview_source_in_us;
  gint64 preview_source_out_us;
  gboolean preview_valid;
} OeTimelineDrag;

struct _OeTimeline
{
  GtkDrawingArea parent_instance;

  OeProject *project;         /* weak — the window owns it */
  OeSequence sequence;        /* deep copy, refreshed per notify */
  gboolean sequence_valid;

  OeTimelineGeometry geometry;
  OeTimelineDrag drag;

  gint selected_track;  /* -1 = no selection */
  gint selected_clip;
  gint64 playhead_us;

  OeTimelineResolveFunc resolve_func;
  gpointer resolve_data;
  OeTimelineReportFunc report_func;
  gpointer report_data;
};

G_DEFINE_TYPE (OeTimeline, oe_timeline, GTK_TYPE_DRAWING_AREA)

/* ------------------------------------------------------------------ */
/* Palette and small cairo helpers.                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
  gdouble r, g, b;
} OeRgba;

static const OeRgba COLOR_BACKGROUND = { 0.114, 0.114, 0.125 };
static const OeRgba COLOR_RULER = { 0.145, 0.145, 0.157 };
static const OeRgba COLOR_LANE_A = { 0.125, 0.125, 0.137 };
static const OeRgba COLOR_LANE_B = { 0.102, 0.102, 0.110 };
static const OeRgba COLOR_LANE_BORDER = { 0.220, 0.220, 0.235 };
static const OeRgba COLOR_CLIP_VIDEO = { 0.227, 0.431, 0.647 };
static const OeRgba COLOR_CLIP_AUDIO = { 0.184, 0.561, 0.357 };
static const OeRgba COLOR_CLIP_STILL = { 0.478, 0.373, 0.627 };
static const OeRgba COLOR_CLIP_BORDER = { 0.066, 0.066, 0.075 };
static const OeRgba COLOR_EDGE_BAND = { 0.980, 0.816, 0.360 };
static const OeRgba COLOR_HATCH = { 0.541, 0.227, 0.227 };
static const OeRgba COLOR_MISSING_BODY = { 0.200, 0.140, 0.140 };
static const OeRgba COLOR_SELECTION = { 1.000, 0.820, 0.360 };
static const OeRgba COLOR_PLAYHEAD = { 0.949, 0.298, 0.298 };
static const OeRgba COLOR_TICK = { 0.361, 0.361, 0.384 };
static const OeRgba COLOR_TEXT = { 0.816, 0.816, 0.827 };
static const OeRgba COLOR_EMPTY_TEXT = { 0.549, 0.549, 0.569 };

static void
set_source (cairo_t *cr, const OeRgba *color)
{
  cairo_set_source_rgb (cr, color->r, color->g, color->b);
}

static void
fill_rect (cairo_t *cr, const OeRgba *color, gdouble x, gdouble y, gdouble w, gdouble h)
{
  if (w <= 0.0 || h <= 0.0)
    return;

  cairo_set_source_rgb (cr, color->r, color->g, color->b);
  cairo_rectangle (cr, x, y, w, h);
  cairo_fill (cr);
}

static void
stroke_rect (cairo_t *cr, const OeRgba *color, gdouble x, gdouble y, gdouble w, gdouble h)
{
  if (w <= 0.0 || h <= 0.0)
    return;

  cairo_set_source_rgb (cr, color->r, color->g, color->b);
  cairo_rectangle (cr, x, y, w, h);
  cairo_set_line_width (cr, 1.0);
  cairo_stroke (cr);
}

/* Diagonal hatch inside a rect: the missing-media state. */
static void
paint_hatch (cairo_t *cr, const OeRgba *color, gdouble x, gdouble y, gdouble w, gdouble h)
{
  if (w <= 0.0 || h <= 0.0)
    return;

  cairo_save (cr);
  cairo_rectangle (cr, x, y, w, h);
  cairo_clip (cr);
  cairo_set_source_rgb (cr, color->r, color->g, color->b);
  cairo_set_line_width (cr, 1.0);

  for (gdouble offset = -h; offset < w; offset += 8.0)
    {
      cairo_move_to (cr, x + offset, y + h);
      cairo_line_to (cr, x + offset + h, y);
    }

  cairo_stroke (cr);
  cairo_restore (cr);
}

static void
draw_text (cairo_t *cr, const OeRgba *color, gdouble x, gdouble y, const gchar *text)
{
  cairo_set_source_rgb (cr, color->r, color->g, color->b);
  cairo_select_font_face (cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, 11.0);
  cairo_move_to (cr, x, y);
  cairo_show_text (cr, text);
}

static void
draw_text_centered (cairo_t *cr, const OeRgba *color, gdouble cx, gdouble cy, const gchar *text)
{
  cairo_text_extents_t extents;

  cairo_set_source_rgb (cr, color->r, color->g, color->b);
  cairo_select_font_face (cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, 12.0);
  cairo_text_extents (cr, text, &extents);
  cairo_move_to (cr, cx - extents.width / 2.0, cy - extents.height / 2.0);
  cairo_show_text (cr, text);
}

/* ------------------------------------------------------------------ */
/* Sequence snapshot (observer → deep copy).                           */
/* ------------------------------------------------------------------ */

static void
invalidate_snapshot (OeTimeline *self)
{
  if (self->sequence_valid)
    {
      oe_sequence_clear (&self->sequence);
      self->sequence_valid = FALSE;
    }
}

static void
refresh_snapshot (OeTimeline *self)
{
  invalidate_snapshot (self);

  if (self->project != NULL)
    {
      oe_sequence_init (&self->sequence);
      oe_project_get_sequence (self->project, &self->sequence);
      self->sequence_valid = TRUE;
    }

  self->geometry.track_count = self->sequence_valid ? self->sequence.tracks->len : 0;

  /* The selection names indices: validate against the fresh copy. */
  if (self->selected_track >= 0)
    {
      OeTrack *track = self->sequence_valid && (guint) self->selected_track < self->sequence.tracks->len
                           ? g_ptr_array_index (self->sequence.tracks, (guint) self->selected_track)
                           : NULL;

      if (track == NULL || (guint) self->selected_clip >= track->clips->len)
        {
          self->selected_track = -1;
          self->selected_clip = -1;
        }
    }

  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* The project observer: fires exactly once per successful mutation,
 * on the calling thread (the model is single-threaded by contract). */
static void
on_project_changed (gpointer user_data)
{
  refresh_snapshot (OE_TIMELINE (user_data));
}

void
oe_timeline_set_project (OeTimeline *self, OeProject *project)
{
  g_return_if_fail (OE_IS_TIMELINE (self));

  if (self->project == project)
    return;

  if (self->project != NULL)
    oe_project_set_observer (self->project, NULL, NULL);

  self->project = project;

  if (self->project != NULL)
    oe_project_set_observer (self->project, on_project_changed, self);

  /* A session swap invalidates every index-based session state. */
  self->drag.kind = DRAG_NONE;
  self->drag.preview_valid = FALSE;
  self->selected_track = -1;
  self->selected_clip = -1;
  self->playhead_us = 0;

  refresh_snapshot (self);
}

void
oe_timeline_set_resolve_func (OeTimeline *self, OeTimelineResolveFunc func, gpointer user_data)
{
  g_return_if_fail (OE_IS_TIMELINE (self));
  self->resolve_func = func;
  self->resolve_data = user_data;
}

void
oe_timeline_set_report_func (OeTimeline *self, OeTimelineReportFunc func, gpointer user_data)
{
  g_return_if_fail (OE_IS_TIMELINE (self));
  self->report_func = func;
  self->report_data = user_data;
}

static void
report (OeTimeline *self, const gchar *message)
{
  if (self->report_func != NULL)
    self->report_func (message, self->report_data);
}

static OeTimelineMediaInfo
resolve_media (OeTimeline *self, guint media_ref)
{
  OeTimelineMediaInfo info = { TRUE, FALSE, FALSE };

  if (self->resolve_func != NULL)
    self->resolve_func (media_ref, &info, self->resolve_data);

  return info;
}

gboolean
oe_timeline_get_selection (OeTimeline *self, guint *track_index, guint *clip_index)
{
  g_return_val_if_fail (OE_IS_TIMELINE (self), FALSE);

  if (self->selected_track < 0)
    return FALSE;

  if (track_index != NULL)
    *track_index = (guint) self->selected_track;
  if (clip_index != NULL)
    *clip_index = (guint) self->selected_clip;
  return TRUE;
}

void
oe_timeline_clear_selection (OeTimeline *self)
{
  g_return_if_fail (OE_IS_TIMELINE (self));

  self->selected_track = -1;
  self->selected_clip = -1;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gdouble
oe_timeline_get_zoom (OeTimeline *self)
{
  g_return_val_if_fail (OE_IS_TIMELINE (self), 0.0);
  return self->geometry.px_per_us;
}

/* Zoom keeping the microsecond under @anchor_x fixed on screen. */
static void
apply_zoom (OeTimeline *self, gdouble new_zoom, gdouble anchor_x)
{
  gint64 anchor_us = oe_timeline_us_for_x (&self->geometry, anchor_x);

  self->geometry.px_per_us = CLAMP (new_zoom, OE_TIMELINE_ZOOM_MIN, OE_TIMELINE_ZOOM_MAX);
  self->geometry.origin_x = MAX (OE_TIMELINE_LEFT_MARGIN, anchor_x - (gdouble) anchor_us * self->geometry.px_per_us);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
oe_timeline_zoom_in (OeTimeline *self)
{
  g_return_if_fail (OE_IS_TIMELINE (self));

  gdouble width = MAX (1.0, (gdouble) gtk_widget_get_width (GTK_WIDGET (self)));

  apply_zoom (self, self->geometry.px_per_us * 2.0, width / 2.0);
}

void
oe_timeline_zoom_out (OeTimeline *self)
{
  g_return_if_fail (OE_IS_TIMELINE (self));

  gdouble width = MAX (1.0, (gdouble) gtk_widget_get_width (GTK_WIDGET (self)));

  apply_zoom (self, self->geometry.px_per_us / 2.0, width / 2.0);
}

gint64
oe_timeline_get_playhead (OeTimeline *self)
{
  g_return_val_if_fail (OE_IS_TIMELINE (self), 0);
  return self->playhead_us;
}

void
oe_timeline_set_playhead (OeTimeline *self, gint64 playhead_us)
{
  g_return_if_fail (OE_IS_TIMELINE (self));

  if (playhead_us < 0)
    playhead_us = 0;

  self->playhead_us = playhead_us;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ------------------------------------------------------------------ */
/* Painting.                                                           */
/* ------------------------------------------------------------------ */

/* Major ruler steps (µs) that stay legible across the zoom range. */
static const gint64 ruler_steps[] = {
  100000, 250000, 500000, 1000000, 2000000, 5000000, 10000000, 30000000, 60000000, 300000000,
};

static gint64
choose_ruler_step (gdouble px_per_us)
{
  for (guint i = 0; i < G_N_ELEMENTS (ruler_steps); i++)
    {
      if ((gdouble) ruler_steps[i] * px_per_us >= 70.0)
        return ruler_steps[i];
    }

  return ruler_steps[G_N_ELEMENTS (ruler_steps) - 1];
}

static void
format_ruler_label (gint64 us, gint64 step, gchar *out, gsize out_len)
{
  if (step < 1000000)
    {
      /* Sub-second steps show a tenths field. */
      g_snprintf (out, out_len, "%lld.%llds", (long long) (us / 1000000),
                  (long long) ((us % 1000000) / 100000));
    }
  else
    {
      g_snprintf (out, out_len, "%lld:%02lld", (long long) (us / 60000000),
                  (long long) ((us / 1000000) % 60));
    }
}

static void
paint_ruler (cairo_t *cr, OeTimeline *self, gdouble width)
{
  fill_rect (cr, &COLOR_RULER, 0, 0, width, OE_TIMELINE_RULER_HEIGHT);

  gint64 step = choose_ruler_step (self->geometry.px_per_us);
  gint64 minor = step / 5;
  gdouble first_x = self->geometry.origin_x;

  /* Minor ticks. */
  set_source (cr, &COLOR_TICK);
  cairo_set_line_width (cr, 1.0);

  for (gint64 us = 0, x = first_x; x <= width; us += minor, x = oe_timeline_x_for_us (&self->geometry, us))
    {
      cairo_move_to (cr, x + 0.5, OE_TIMELINE_RULER_HEIGHT - 5);
      cairo_line_to (cr, x + 0.5, OE_TIMELINE_RULER_HEIGHT);
    }

  cairo_stroke (cr);

  /* Major ticks + labels. */
  for (gint64 us = 0, x = first_x; x <= width; us += step, x = oe_timeline_x_for_us (&self->geometry, us))
    {
      set_source (cr, &COLOR_TICK);
      cairo_set_line_width (cr, 1.0);
      cairo_move_to (cr, x + 0.5, OE_TIMELINE_RULER_HEIGHT - 10);
      cairo_line_to (cr, x + 0.5, OE_TIMELINE_RULER_HEIGHT);
      cairo_stroke (cr);

      gchar label[32];

      format_ruler_label (us, step, label, sizeof (label));
      draw_text (cr, &COLOR_TEXT, x + 3, 11, label);
    }
}

static void
paint_lanes (cairo_t *cr, OeTimeline *self, gdouble width, gdouble height)
{
  for (guint i = 0; i < self->geometry.track_count; i++)
    {
      gdouble y = oe_timeline_y_for_track (&self->geometry, i);

      if (y >= height)
        break;

      fill_rect (cr, i % 2 == 0 ? &COLOR_LANE_A : &COLOR_LANE_B, 0, y, width, OE_TIMELINE_TRACK_HEIGHT);
      stroke_rect (cr, &COLOR_LANE_BORDER, 0, y + 0.5, width, OE_TIMELINE_TRACK_HEIGHT - 1);
    }
}

/* Paints one clip body, resolving the drag preview when this clip is
 * the one being dragged. */
static void
paint_clip (cairo_t *cr, OeTimeline *self, guint track_index, guint clip_index, const OeClip *clip)
{
  OeTimelineMediaInfo media = resolve_media (self, clip->media_ref);
  gint64 position = clip->position_us;
  gint64 source_in = clip->source_in_us;
  gint64 source_out = clip->source_out_us;
  gboolean previewing = self->drag.preview_valid && self->drag.kind != DRAG_PLAYHEAD
                        && self->drag.track_index == track_index && self->drag.clip_index == clip_index;

  if (previewing)
    {
      if (self->drag.kind == DRAG_MOVE)
        position = self->drag.preview_position_us;
      else if (self->drag.kind == DRAG_TRIM_IN)
        source_in = self->drag.preview_source_in_us;
      else
        source_out = self->drag.preview_source_out_us;
    }

  gdouble x = oe_timeline_x_for_us (&self->geometry, position);
  gdouble w = oe_timeline_x_for_us (&self->geometry, position + (source_out - source_in)) - x;
  gdouble y = oe_timeline_y_for_track (&self->geometry, track_index);
  gdouble body_h = media.has_audio ? OE_TIMELINE_TRACK_HEIGHT * 0.6 : OE_TIMELINE_TRACK_HEIGHT;

  if (x + w < 0.0)
    return;

  const OeRgba *body = media.missing ? &COLOR_MISSING_BODY
                                     : (media.is_still ? &COLOR_CLIP_STILL : &COLOR_CLIP_VIDEO);

  fill_rect (cr, body, x, y, w, body_h);

  if (media.has_audio)
    fill_rect (cr, &COLOR_CLIP_AUDIO, x, y + body_h, w, OE_TIMELINE_TRACK_HEIGHT - body_h);

  if (media.missing)
    paint_hatch (cr, &COLOR_HATCH, x, y, w, OE_TIMELINE_TRACK_HEIGHT);

  /* Edge-grab bands: the trim affordance at both ends. */
  if (!media.missing)
    {
      fill_rect (cr, &COLOR_EDGE_BAND, x, y, OE_TIMELINE_EDGE_BAND_PX, OE_TIMELINE_TRACK_HEIGHT);
      fill_rect (cr, &COLOR_EDGE_BAND, x + w - OE_TIMELINE_EDGE_BAND_PX, y, OE_TIMELINE_EDGE_BAND_PX,
                 OE_TIMELINE_TRACK_HEIGHT);
    }

  stroke_rect (cr, &COLOR_CLIP_BORDER, x + 0.5, y + 0.5, w - 1.0, OE_TIMELINE_TRACK_HEIGHT - 1.0);

  if (self->selected_track >= 0 && (guint) self->selected_track == track_index
      && (guint) self->selected_clip == clip_index)
    stroke_rect (cr, &COLOR_SELECTION, x + 1.0, y + 1.0, w - 2.0, OE_TIMELINE_TRACK_HEIGHT - 2.0);

  /* Label: basename of the media path, drawn inside the body. */
  gchar *path = self->project != NULL ? oe_project_dup_media_path (self->project, clip->media_ref) : NULL;

  if (path != NULL)
    {
      g_autofree gchar *basename = g_path_get_basename (path);
      cairo_text_extents_t extents;

      cairo_select_font_face (cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size (cr, 11.0);
      cairo_text_extents (cr, basename, &extents);

      if (extents.width + 12.0 < w)
        {
          cairo_save (cr);
          cairo_rectangle (cr, x, y, w, body_h);
          cairo_clip (cr);
          draw_text (cr, &COLOR_TEXT, x + 8, y + body_h - 6, basename);
          cairo_restore (cr);
        }

      g_free (path);
    }
}

static void
paint_clips (cairo_t *cr, OeTimeline *self, gdouble width G_GNUC_UNUSED)
{
  if (!self->sequence_valid)
    return;

  for (guint t = 0; t < self->sequence.tracks->len; t++)
    {
      OeTrack *track = g_ptr_array_index (self->sequence.tracks, t);

      for (guint c = 0; c < track->clips->len; c++)
        paint_clip (cr, self, t, c, g_ptr_array_index (track->clips, c));
    }
}

static void
paint_playhead (cairo_t *cr, OeTimeline *self, gdouble height)
{
  gdouble x = oe_timeline_x_for_us (&self->geometry, self->playhead_us);

  set_source (cr, &COLOR_PLAYHEAD);
  cairo_set_line_width (cr, 1.5);
  cairo_move_to (cr, x, 0);
  cairo_line_to (cr, x, height);
  cairo_stroke (cr);

  /* Ruler marker: a small triangle at the top. */
  cairo_move_to (cr, x - 5, 0);
  cairo_line_to (cr, x + 5, 0);
  cairo_line_to (cr, x, 8);
  cairo_close_path (cr);
  cairo_fill (cr);
}

static void
paint_empty_state (cairo_t *cr, OeTimeline *self, gdouble width, gdouble height)
{
  const gchar *text = self->geometry.track_count == 0
                          ? "Drop media here — import via Media ▸ Import Media"
                          : "Add clips via Media ▸ Insert from Bin";

  draw_text_centered (cr, &COLOR_EMPTY_TEXT, width / 2.0, height / 2.0, text);
}

static void
draw_frame (GtkDrawingArea *area G_GNUC_UNUSED, cairo_t *cr, int width, int height, gpointer user_data)
{
  OeTimeline *self = OE_TIMELINE (user_data);

  fill_rect (cr, &COLOR_BACKGROUND, 0, 0, width, height);

  if (self->geometry.track_count == 0)
    {
      paint_ruler (cr, self, width);
      paint_empty_state (cr, self, width, height);
      return;
    }

  paint_lanes (cr, self, width, height);
  paint_clips (cr, self, width);
  paint_ruler (cr, self, width);
  paint_playhead (cr, self, height);
}

/* ------------------------------------------------------------------ */
/* Interaction: one drag state machine shared by the controllers.      */
/* ------------------------------------------------------------------ */

static void
cancel_drag (OeTimeline *self)
{
  self->drag.kind = DRAG_NONE;
  self->drag.preview_valid = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
find_clip_bounds (OeTimeline *self, guint track_index, guint clip_index, OeClip *out)
{
  if (!self->sequence_valid || track_index >= self->sequence.tracks->len)
    return FALSE;

  OeTrack *track = g_ptr_array_index (self->sequence.tracks, track_index);

  if (clip_index >= track->clips->len)
    return FALSE;

  *out = *(OeClip *) g_ptr_array_index (track->clips, clip_index);
  return TRUE;
}

/* Press: hit-test and arm the drag. Returns TRUE when armed. */
static void
arm_drag (OeTimeline *self, gdouble x, gdouble y)
{
  OeTimelineHit hit = oe_timeline_hit_test (&self->geometry, &self->sequence, x, y);

  self->drag.press_x = x;
  self->drag.press_y = y;
  self->drag.preview_valid = FALSE;
  self->drag.kind = DRAG_NONE;

  switch (hit.kind)
    {
    case OE_TIMELINE_HIT_RULER:
      self->drag.kind = DRAG_PLAYHEAD;
      self->playhead_us = oe_timeline_us_for_x (&self->geometry, x);
      break;

    case OE_TIMELINE_HIT_MOVE:
    case OE_TIMELINE_HIT_TRIM_IN:
    case OE_TIMELINE_HIT_TRIM_OUT:
      {
        OeClip clip;

        if (!find_clip_bounds (self, hit.track_index, hit.clip_index, &clip))
          return;

        /* Select whatever clip the press landed on. */
        self->selected_track = (gint) hit.track_index;
        self->selected_clip = (gint) hit.clip_index;

        OeTimelineMediaInfo media = resolve_media (self, clip.media_ref);

        if (hit.kind != OE_TIMELINE_HIT_MOVE && media.missing)
          {
            /* Missing media renders hatched and refuses trims. */
            report (self, "Missing media cannot be trimmed");
            return;
          }

        self->drag.track_index = hit.track_index;
        self->drag.clip_index = hit.clip_index;

        if (hit.kind == OE_TIMELINE_HIT_MOVE)
          {
            self->drag.kind = DRAG_MOVE;
            self->drag.grab_offset_us = oe_timeline_us_for_x (&self->geometry, x) - clip.position_us;
          }
        else if (hit.kind == OE_TIMELINE_HIT_TRIM_IN)
          {
            self->drag.kind = DRAG_TRIM_IN;
            self->drag.preview_source_in_us = clip.source_in_us;
          }
        else
          {
            self->drag.kind = DRAG_TRIM_OUT;
            self->drag.preview_source_out_us = clip.source_out_us;
          }
      }
      break;

    case OE_TIMELINE_HIT_NOTHING:
    default:
      /* Armed as NOTHING: release clears the selection. */
      self->selected_track = -1;
      self->selected_clip = -1;
      break;
    }
}

/* Motion: preview the clamped candidate (never writes the model). */
static void
update_drag (OeTimeline *self, gdouble x)
{
  switch (self->drag.kind)
    {
    case DRAG_PLAYHEAD:
      self->playhead_us = oe_timeline_us_for_x (&self->geometry, x);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      break;

    case DRAG_MOVE:
      {
        OeClip clip;

        if (!find_clip_bounds (self, self->drag.track_index, self->drag.clip_index, &clip))
          {
            cancel_drag (self);
            return;
          }

        gint64 wanted = oe_timeline_us_for_x (&self->geometry, x) - self->drag.grab_offset_us;

        self->drag.preview_position_us
            = oe_timeline_clamp_move_position (&self->sequence, self->drag.track_index,
                                               self->drag.clip_index, wanted);
        self->drag.preview_valid = TRUE;
        gtk_widget_queue_draw (GTK_WIDGET (self));
      }
      break;

    case DRAG_TRIM_IN:
    case DRAG_TRIM_OUT:
      {
        OeClip clip;

        if (!find_clip_bounds (self, self->drag.track_index, self->drag.clip_index, &clip))
          {
            cancel_drag (self);
            return;
          }

        gint64 min_in = 0, max_in = 0, min_out = 0, max_out = 0;
        gint64 max_source_us = 0;

        if (self->project != NULL)
          oe_project_get_media_source_duration (self->project, clip.media_ref, &max_source_us);

        oe_timeline_trim_bounds (&clip, max_source_us, &min_in, &max_in, &min_out, &max_out);

        gint64 wanted = oe_timeline_us_for_x (&self->geometry, x);

        if (self->drag.kind == DRAG_TRIM_IN)
          self->drag.preview_source_in_us = CLAMP (wanted, min_in, max_in);
        else
          self->drag.preview_source_out_us = CLAMP (wanted, min_out, max_out);

        self->drag.preview_valid = TRUE;
        gtk_widget_queue_draw (GTK_WIDGET (self));
      }
      break;

    case DRAG_NONE:
    default:
      break;
    }
}

/* Release: commit through the model mutators. */
static void
commit_drag (OeTimeline *self, gdouble total_dx, gdouble total_dy)
{
  gdouble distance = sqrt (total_dx * total_dx + total_dy * total_dy);
  OeTimelineDragKind kind = self->drag.kind;

  self->drag.preview_valid = FALSE;

  if (distance < OE_TIMELINE_CLICK_THRESHOLD || self->project == NULL)
    {
      /* A click: selection/playhead already happened at press. */
      self->drag.kind = DRAG_NONE;
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  switch (kind)
    {
    case DRAG_MOVE:
      {
        GError *error = NULL;

        if (!oe_project_move_clip (self->project, self->drag.track_index, self->drag.clip_index,
                                   self->drag.preview_position_us, &error))
          {
            g_autofree gchar *msg = g_strdup_printf ("Move rejected: %s", error->message);

            report (self, msg); /* snap-back: the preview simply clears */
            g_error_free (error);
          }
      }
      break;

    case DRAG_TRIM_IN:
    case DRAG_TRIM_OUT:
      {
        OeClip clip;
        GError *error = NULL;

        if (!find_clip_bounds (self, self->drag.track_index, self->drag.clip_index, &clip))
          break;

        gint64 new_in = self->drag.kind == DRAG_TRIM_IN ? self->drag.preview_source_in_us : clip.source_in_us;
        gint64 new_out = self->drag.kind == DRAG_TRIM_OUT ? self->drag.preview_source_out_us : clip.source_out_us;

        if (!oe_project_trim_clip (self->project, self->drag.track_index, self->drag.clip_index, new_in, new_out,
                                   &error))
          {
            g_autofree gchar *msg = g_strdup_printf ("Trim rejected: %s", error->message);

            report (self, msg); /* snap-back: the preview simply clears */
            g_error_free (error);
          }
      }
      break;

    case DRAG_PLAYHEAD:
      /* Session playhead: no model write (Phase 5 owns the clock). */
      break;

    case DRAG_NONE:
    default:
      break;
    }

  self->drag.kind = DRAG_NONE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_begin (GtkGestureDrag *gesture G_GNUC_UNUSED, gdouble start_x, gdouble start_y, gpointer user_data)
{
  OeTimeline *self = OE_TIMELINE (user_data);

  /* Motion events flow through the drag gesture's update; arm from
   * the press point. */
  arm_drag (self, start_x, start_y);

  if (self->drag.kind == DRAG_NONE && self->selected_track < 0)
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_update (GtkGestureDrag *gesture G_GNUC_UNUSED, gdouble offset_x, gdouble offset_y G_GNUC_UNUSED, gpointer user_data)
{
  OeTimeline *self = OE_TIMELINE (user_data);

  if (self->drag.kind == DRAG_NONE)
    return;

  update_drag (self, self->drag.press_x + offset_x);
}

static void
on_drag_end (GtkGestureDrag *gesture G_GNUC_UNUSED, gdouble offset_x, gdouble offset_y, gpointer user_data)
{
  OeTimeline *self = OE_TIMELINE (user_data);

  commit_drag (self, offset_x, offset_y);
}

static gboolean
on_scroll (GtkEventControllerScroll *controller, gdouble dx, gdouble dy, gpointer user_data)
{
  OeTimeline *self = OE_TIMELINE (user_data);
  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (controller));

  if ((state & GDK_CONTROL_MASK) != 0)
    {
      /* Ctrl+wheel: double/halve the zoom around the pointer. */
      gdouble x = 0.0, y = 0.0;
      gdouble px_per_us = self->geometry.px_per_us;

      /* GTK4 has no controller-position getter; read it off the
       * current event instead. */
      GdkEvent *event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller));
      if (event != NULL)
        gdk_event_get_position (event, &x, &y);
      apply_zoom (self, dy < 0 ? px_per_us * 2.0 : px_per_us / 2.0, x);
    }
  else
    {
      /* Plain wheel: horizontal pan, clamped at the left margin. */
      self->geometry.origin_x = MAX (OE_TIMELINE_LEFT_MARGIN, self->geometry.origin_x - dx - dy);
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }

  return GDK_EVENT_STOP;
}

/* ------------------------------------------------------------------ */
/* Type boilerplate.                                                   */
/* ------------------------------------------------------------------ */

static void
oe_timeline_dispose (GObject *object)
{
  OeTimeline *self = OE_TIMELINE (object);

  if (self->project != NULL)
    {
      oe_project_set_observer (self->project, NULL, NULL);
      self->project = NULL;
    }

  invalidate_snapshot (self);

  G_OBJECT_CLASS (oe_timeline_parent_class)->dispose (object);
}

static void
oe_timeline_class_init (OeTimelineClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = oe_timeline_dispose;
}

static void
oe_timeline_init (OeTimeline *self)
{
  oe_sequence_init (&self->sequence);
  self->geometry.px_per_us = OE_TIMELINE_ZOOM_DEFAULT;
  self->geometry.origin_x = OE_TIMELINE_LEFT_MARGIN;
  self->geometry.track_count = 0;
  self->drag.kind = DRAG_NONE;
  self->selected_track = -1;
  self->selected_clip = -1;

  gtk_widget_add_css_class (GTK_WIDGET (self), "timeline");
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);

  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self), draw_frame, self, NULL);

  /* One state machine, three controllers: drag (press/motion/release),
   * scroll (pan + Ctrl-wheel zoom). */
  GtkGestureDrag *drag = GTK_GESTURE_DRAG (gtk_gesture_drag_new ());

  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

  GtkEventControllerScroll *scroll
      = GTK_EVENT_CONTROLLER_SCROLL (gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES));

  g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (scroll));
}

OeTimeline *
oe_timeline_new (void)
{
  return g_object_new (OE_TYPE_TIMELINE, NULL);
}
