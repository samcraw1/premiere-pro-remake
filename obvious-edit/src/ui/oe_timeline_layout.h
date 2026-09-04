/* oe_timeline_layout.h — GTK-free timeline geometry (Phase 4).
 *
 * Pure functions over plain structs: mapping microseconds to pixels and
 * back, deciding what a pointer press lands on, and bounding drags so
 * previews stay inside what the project model would accept. No GTK
 * types, no clocks, no side effects — the widget owns session state
 * and applies the verdicts through the project's mutators.
 *
 * Every function is total: it answers for every input, so event
 * handlers never re-implement the edge cases.
 */

#pragma once

#include <glib.h>

#include "../core/oe_project.h"

G_BEGIN_DECLS

/* Horizontal layout constants (pixels). The edge band reaches inward
 * from a clip's edges: a press near an edge arms a trim, a press in
 * the body arms a move. */
#define OE_TIMELINE_RULER_HEIGHT 24
#define OE_TIMELINE_TRACK_HEIGHT 56
#define OE_TIMELINE_EDGE_BAND_PX 6

/* A source range shorter than this is a mistyped click, not a trim. */
#define OE_TIMELINE_MIN_TRIM_US 1

typedef struct
{
  /* Zoom: pixels per microsecond. Session-only — the widget owns it
   * (the model has no field for it; Phase 5 owns the clock and any
   * persistence), doubling/halving it around the pointer. */
  gdouble px_per_us;

  /* Widget x of timeline position 0 (the ruler's left edge). */
  gdouble origin_x;

  /* Number of track lanes below the ruler. */
  guint track_count;
} OeTimelineGeometry;

typedef enum
{
  OE_TIMELINE_HIT_NOTHING,
  OE_TIMELINE_HIT_RULER,
  OE_TIMELINE_HIT_MOVE,
  OE_TIMELINE_HIT_TRIM_IN,
  OE_TIMELINE_HIT_TRIM_OUT,
} OeTimelineHitKind;

typedef struct
{
  OeTimelineHitKind kind;
  guint track_index; /* valid when kind is a clip hit */
  guint clip_index;
} OeTimelineHit;

/* Conversions. us_for_x clamps to 0 left of the origin: the timeline
 * starts at the origin, and a pointer cannot go further left. */
gdouble oe_timeline_x_for_us (const OeTimelineGeometry *geometry, gint64 us);
gint64 oe_timeline_us_for_x (const OeTimelineGeometry *geometry, gdouble x);

/* Lane mapping: track 0 starts under the ruler; track_index_for_y
 * answers -1 on the ruler and past the last lane. */
gdouble oe_timeline_y_for_track (const OeTimelineGeometry *geometry, guint track_index);
gint oe_timeline_track_index_for_y (const OeTimelineGeometry *geometry, gdouble y);

/* What a press at (x, y) lands on: a clip body (MOVE), within the edge
 * band of a clip edge (TRIM_IN/TRIM_OUT), the ruler, or nothing. */
OeTimelineHit oe_timeline_hit_test (const OeTimelineGeometry *geometry, const OeSequence *sequence,
                                    gdouble x, gdouble y);

/* Drag bounds for a move: the nearest legal position at or around
 * @wanted_position on the clip's own track — snapped against every
 * neighbour, never negative, never overlapping. Adjacency is legal. */
gint64 oe_timeline_clamp_move_position (const OeSequence *sequence, guint track_index,
                                        guint clip_index, gint64 wanted_position);

/* Drag bounds for a trim: source-in may slide within
 * [0, source_out - min] and source-out within
 * [source_in + min, max_source_us] — where max_source_us is the probed
 * duration the widget reads from the project's session annotation (0
 * or less means unbounded: stills and unprobed media). */
void oe_timeline_trim_bounds (const OeClip *clip, gint64 max_source_us, gint64 *min_in,
                              gint64 *max_in, gint64 *min_out, gint64 *max_out);

G_END_DECLS
