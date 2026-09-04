/* oe_timeline_layout.c — pure geometry for the timeline widget.
 *
 * All math lives here so the widget stays a thin event loop: this file
 * is GTK-free, total over its inputs, and unit-tested against real
 * project-model copies (see test_timeline_layout, test_snap_ripple).
 *
 * Phase 7: magnetism is a separate pure decision
 * (oe_timeline_snap_time) consumed by the widget's preview path
 * BEFORE legality clamping. The clamp keeps one job — recovering an
 * infeasible candidate to a legal position — and never moves an
 * already-legal candidate, so an on-band release locks flush while an
 * off-band release keeps the raw pointer position.
 */

#include "oe_timeline_layout.h"

#include <math.h>

/* Footprint end, saturated: a still grown to absurd duration is still
 * answerable without overflow. */
static gint64
clip_end_us (const OeClip *clip)
{
  gint64 duration = clip->source_out_us - clip->source_in_us;

  if (clip->position_us > G_MAXINT64 - duration)
    return G_MAXINT64;

  return clip->position_us + duration;
}

/* The dragged clip's own footprint is not an overlap target (the same
 * rule the model applies to moves); other clips are. */
static gboolean
position_fits (const OeTrack *track, const OeClip *mover, gint64 position, gint64 duration)
{
  for (guint i = 0; i < track->clips->len; i++)
    {
      OeClip *other = g_ptr_array_index (track->clips, i);

      if (other == mover)
        continue;

      if (position < clip_end_us (other) && other->position_us < position + duration)
        return FALSE;
    }

  return TRUE;
}

static gint64
abs_diff_us (gint64 a, gint64 b)
{
  return a >= b ? a - b : b - a;
}

/* Saturated |a - b|: distances near G_MAXINT64 saturate instead of
 * wrapping, so a far target can never win by overflow. */
static gint64
snap_distance_us (gint64 a, gint64 b)
{
  if (a >= b)
    return a - b;

  return b - a;
}

/* Threshold band in microseconds, scaled by the current zoom: a band
 * that feels constant on screen widens in time as the view zooms
 * out. */
static gint64
snap_threshold_us (const OeSnapContext *ctx)
{
  if (ctx->px_per_us <= 0.0)
    return 0;

  gdouble band = ctx->threshold_px / ctx->px_per_us;

  if (band >= (gdouble) G_MAXINT64)
    return G_MAXINT64;

  return (gint64) (band + 0.5);
}

/* Nearest-target bookkeeping: a new candidate replaces the incumbent
 * when strictly closer, or equally close and earlier (deterministic
 * tie-break for targets that coincide, e.g. an edge on a frame
 * boundary). */
typedef struct
{
  gint64 best;
  gint64 best_distance;
  gboolean have_best;
} SnapBest;

static void
snap_offer (SnapBest *best, gint64 target, gint64 candidate, gint64 threshold)
{
  gint64 distance = snap_distance_us (target, candidate);

  if (distance > threshold)
    return;

  if (!best->have_best || distance < best->best_distance
      || (distance == best->best_distance && target < best->best))
    {
      best->best = target;
      best->best_distance = distance;
      best->have_best = TRUE;
    }
}

gint64
oe_timeline_snap_time (const OeSnapContext *ctx, gint64 candidate_us)
{
  g_return_val_if_fail (ctx != NULL, candidate_us);

  if (!ctx->enabled || candidate_us < 0)
    return candidate_us;

  gint64 threshold = snap_threshold_us (ctx);

  if (threshold <= 0)
    return candidate_us;

  SnapBest best = { 0, 0, FALSE };

  /* Zero: the timeline's left wall. */
  snap_offer (&best, 0, candidate_us, threshold);

  /* Frame grid: the nearest frame boundary, when the sequence has a
   * frame rate. */
  if (ctx->frame_interval_us > 0)
    {
      gint64 frame = candidate_us / ctx->frame_interval_us;
      gint64 floor_target = frame * ctx->frame_interval_us;
      gint64 ceil_target = floor_target;

      if (floor_target < candidate_us && floor_target <= G_MAXINT64 - ctx->frame_interval_us)
        ceil_target = floor_target + ctx->frame_interval_us;

      snap_offer (&best, floor_target, candidate_us, threshold);
      snap_offer (&best, ceil_target, candidate_us, threshold);
    }

  /* Live playhead. */
  if (ctx->playhead_us >= 0)
    snap_offer (&best, ctx->playhead_us, candidate_us, threshold);

  /* Same-track neighbour clip edges, pre-collected by the widget. */
  for (gsize i = 0; i < ctx->n_edges; i++)
    {
      gint64 edge = ctx->edges_us[i];

      if (edge >= 0)
        snap_offer (&best, edge, candidate_us, threshold);
    }

  if (!best.have_best)
    return candidate_us;

  return best.best;
}

static OeTimelineHit
hit_nothing (void)
{
  OeTimelineHit hit = { OE_TIMELINE_HIT_NOTHING, 0, 0 };

  return hit;
}

gdouble
oe_timeline_x_for_us (const OeTimelineGeometry *geometry, gint64 us)
{
  g_return_val_if_fail (geometry != NULL, 0.0);
  g_return_val_if_fail (geometry->px_per_us > 0.0, 0.0);

  return geometry->origin_x + (gdouble) us * geometry->px_per_us;
}

gint64
oe_timeline_us_for_x (const OeTimelineGeometry *geometry, gdouble x)
{
  g_return_val_if_fail (geometry != NULL, 0);
  g_return_val_if_fail (geometry->px_per_us > 0.0, 0);

  gdouble us = (x - geometry->origin_x) / geometry->px_per_us;

  if (us <= 0.0)
    return 0;

  if (us >= (gdouble) G_MAXINT64)
    return G_MAXINT64;

  /* Round to the nearest microsecond: a half-pixel wobble must not
   * decide whether a trim lands inside its neighbour. */
  return (gint64) (us + 0.5);
}

gdouble
oe_timeline_y_for_track (const OeTimelineGeometry *geometry, guint track_index)
{
  g_return_val_if_fail (geometry != NULL, 0.0);

  return OE_TIMELINE_RULER_HEIGHT + (gdouble) track_index * OE_TIMELINE_TRACK_HEIGHT;
}

gint
oe_timeline_track_index_for_y (const OeTimelineGeometry *geometry, gdouble y)
{
  g_return_val_if_fail (geometry != NULL, -1);

  if (y < OE_TIMELINE_RULER_HEIGHT)
    return -1;

  guint index = (guint) ((y - OE_TIMELINE_RULER_HEIGHT) / OE_TIMELINE_TRACK_HEIGHT);

  if (index >= geometry->track_count)
    return -1;

  return (gint) index;
}

OeTimelineHit
oe_timeline_hit_test (const OeTimelineGeometry *geometry, const OeSequence *sequence, gdouble x,
                      gdouble y)
{
  g_return_val_if_fail (geometry != NULL, hit_nothing ());
  g_return_val_if_fail (geometry->px_per_us > 0.0, hit_nothing ());
  g_return_val_if_fail (sequence != NULL, hit_nothing ());

  gint track_index = oe_timeline_track_index_for_y (geometry, y);

  if (track_index < 0)
    {
      OeTimelineHit hit = hit_nothing ();

      /* The ruler band answers RULER; the dead space past the last
       * lane stays NOTHING. */
      if (y < OE_TIMELINE_RULER_HEIGHT)
        hit.kind = OE_TIMELINE_HIT_RULER;

      return hit;
    }

  OeTrack *track = g_ptr_array_index (sequence->tracks, (guint) track_index);

  for (guint i = 0; i < track->clips->len; i++)
    {
      OeClip *clip = g_ptr_array_index (track->clips, i);
      gdouble left = oe_timeline_x_for_us (geometry, clip->position_us);
      gdouble right = oe_timeline_x_for_us (geometry, clip_end_us (clip));

      if (x < left || x > right)
        continue;

      OeTimelineHit hit = hit_nothing ();

      hit.kind = OE_TIMELINE_HIT_MOVE;
      hit.track_index = (guint) track_index;
      hit.clip_index = i;

      if (x - left <= OE_TIMELINE_EDGE_BAND_PX)
        hit.kind = OE_TIMELINE_HIT_TRIM_IN;
      else if (right - x <= OE_TIMELINE_EDGE_BAND_PX)
        hit.kind = OE_TIMELINE_HIT_TRIM_OUT;

      return hit;
    }

  return hit_nothing ();
}

gint64
oe_timeline_clamp_move_position (const OeSequence *sequence, guint track_index, guint clip_index,
                                 gint64 wanted_position)
{
  g_return_val_if_fail (sequence != NULL, 0);
  g_return_val_if_fail (track_index < sequence->tracks->len, 0);

  OeTrack *track = g_ptr_array_index (sequence->tracks, track_index);

  g_return_val_if_fail (clip_index < track->clips->len, 0);

  OeClip *clip = g_ptr_array_index (track->clips, clip_index);
  gint64 duration = clip->source_out_us - clip->source_in_us;

  /* Legality only: a wanted position that already fits passes through
   * unchanged — magnetism is oe_timeline_snap_time()'s job now. An
   * infeasible wanted position recovers to the nearest legal contact:
   * the hard-left floor, or whichever neighbour edge (its end, or its
   * start minus the mover's duration) is closest to what was asked.
   * Ties prefer the smaller position. */
  if (wanted_position >= 0 && position_fits (track, clip, wanted_position, duration))
    return wanted_position;

  gint64 best = 0;
  gint64 best_distance = G_MAXINT64;
  gboolean have_best = FALSE;

  for (guint pass = 0; pass < 2; pass++)
    {
      gint64 candidate = pass == 0 ? wanted_position : 0;

      if (candidate < 0)
        candidate = 0;

      if (position_fits (track, clip, candidate, duration)
          && (!have_best || abs_diff_us (candidate, wanted_position) < best_distance
              || (abs_diff_us (candidate, wanted_position) == best_distance && candidate < best)))
        {
          best = candidate;
          best_distance = abs_diff_us (candidate, wanted_position);
          have_best = TRUE;
        }
    }

  for (guint i = 0; i < track->clips->len; i++)
    {
      OeClip *other = g_ptr_array_index (track->clips, i);

      if (other == clip)
        continue;

      gint64 candidates[2] = { clip_end_us (other), other->position_us - duration };

      for (guint c = 0; c < 2; c++)
        {
          gint64 candidate = candidates[c];

          if (candidate < 0)
            continue;

          if (position_fits (track, clip, candidate, duration)
              && (!have_best || abs_diff_us (candidate, wanted_position) < best_distance
                  || (abs_diff_us (candidate, wanted_position) == best_distance
                      && candidate < best)))
            {
              best = candidate;
              best_distance = abs_diff_us (candidate, wanted_position);
              have_best = TRUE;
            }
        }
    }

  return best;
}

void
oe_timeline_trim_bounds (const OeClip *clip, gint64 max_source_us, gint64 *min_in, gint64 *max_in,
                         gint64 *min_out, gint64 *max_out)
{
  g_return_if_fail (clip != NULL);

  gint64 ceiling = max_source_us > 0 ? max_source_us : G_MAXINT64;

  if (min_in != NULL)
    *min_in = 0;

  if (max_in != NULL)
    *max_in = clip->source_out_us - OE_TIMELINE_MIN_TRIM_US;

  if (min_out != NULL)
    *min_out = clip->source_in_us + OE_TIMELINE_MIN_TRIM_US;

  if (max_out != NULL)
    *max_out = ceiling;
}
