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

/* Phase 7: magnetism band for snapping, in pixels. The default sits
 * just outside the 6 px trim edge band: close enough to feel
 * magnetic, far enough not to eat trim presses. */
#define OE_TIMELINE_SNAP_THRESHOLD_PX 8.0

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

/* Phase 7: pure snapping. The context carries everything the snap
 * decision needs — the toggle, the px threshold, the current zoom, the
 * playhead, the frame interval, and the dragged clip's same-track
 * neighbour edges — so the decision itself stays free of the widget,
 * the model, and GTK.
 *
 * Distances are compared in TIME, but the threshold is in PIXELS: a
 * band that feels constant on screen must widen in microseconds as
 * the view zooms out. px_per_us does that conversion.
 */
typedef struct
{
  gboolean enabled;         /* session toggle; FALSE = raw candidate */
  gdouble threshold_px;     /* magnetism band, ~6–10 px */
  gdouble px_per_us;        /* current zoom (pixels per µs) */
  gint64 playhead_us;       /* live playhead position (target) */
  gint64 frame_interval_us; /* 1000000 / frame_rate; 0 = grid off */

  /* Same-track neighbour clip edges, pre-collected: both edges of
   * every other clip on the dragged clip's track. The snap core never
   * touches the model — the widget owns the collection. */
  const gint64 *edges_us;
  gsize n_edges;
} OeSnapContext;

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

/* Phase 7: returns the snapped time for a drag candidate, or
 * @candidate_us unchanged when snapping is disabled or no target lies
 * within the threshold band. Targets: the same-track neighbour clip
 * edges in @edges_us, the playhead, zero, and the frame grid
 * (frame_interval_us == 0 disables the grid). Nearest target wins;
 * ties go to the earlier time. Pure: no widget, no model, no
 * allocation. Overflow-safe: distances saturate instead of wrapping. */
gint64 oe_timeline_snap_time (const OeSnapContext *ctx, gint64 candidate_us);

/* Drag bounds for a move: the nearest legal position at or around
 * @wanted_position on the clip's own track — snapped against every
 * neighbour, never negative, never overlapping. Adjacency is legal.
 *
 * Phase 7 splits the two jobs this function used to fuse: legality
 * recovery stays here (an infeasible wanted position recovers to the
 * nearest legal floor or neighbour contact), while magnetism moved to
 * oe_timeline_snap_time() — which the widget applies BEFORE calling
 * this, so an on-band release locks flush but an off-band release
 * keeps its raw position. */
gint64 oe_timeline_clamp_move_position (const OeSequence *sequence, guint track_index,
                                        guint clip_index, gint64 wanted_position);

/* Phase 9 Wave B: transition boundary bands. The band's horizontal
 * extent is the transition's EFFECTIVE window, re-derived from the
 * current clips (an inactive window draws no band and offers no snap
 * targets). Pure GTK-free geometry: the widget just draws. */
typedef struct
{
  gboolean active;
  gdouble x_start;
  gdouble x_end;
  gdouble y;      /* top of the track lane */
  gdouble height; /* lane height */
} OeTransitionBandRect;

OeTransitionBandRect oe_timeline_transition_band (const OeTimelineGeometry *geometry,
                                                  const OeTransitionWindow *window,
                                                  guint track_index);

/* Phase 9 Wave B: the two edge targets a transition contributes to
 * snapping — its effective window's start and end. Returns FALSE when
 * the window is inactive: no edges. The snap core never touches the
 * model; callers extend the pre-collected edge list with these. */
gboolean oe_timeline_transition_edges (const OeTransitionWindow *window, gint64 *start_us,
                                       gint64 *end_us);

/* Drag bounds for a trim: source-in may slide within
 * [0, source_out - min] and source-out within
 * [source_in + min, max_source_us] — where max_source_us is the probed
 * duration the widget reads from the project's session annotation (0
 * or less means unbounded: stills and unprobed media). */
void oe_timeline_trim_bounds (const OeClip *clip, gint64 max_source_us, gint64 *min_in,
                              gint64 *max_in, gint64 *min_out, gint64 *max_out);

/* Phase 11 Wave B: the model-side label a timeline clip draws — GTK-free
 * so tests pin it at the model seam. Generated clips label from their
 * own payload (the title's text, or the solid's packed color); media
 * clips label from the media path's basename (NULL path → NULL: the
 * caller draws no label, the missing-media hatch speaks instead). A
 * title with empty text falls back to "Title" so an empty entry can
 * never erase the label.
 *
 * Returns a heap string (transfer full), NULL when there is no label. */
gchar *oe_timeline_clip_label (const OeClip *clip, const gchar *media_path);

/* Phase 11 Wave B: packed-0xRRGGBB ↔ "#rrggbb" text. The hex spelling
 * is shared by the timeline's solid labels and the inspector's color
 * entries, so both spellings always agree. The parser accepts an
 * optional leading '#' followed by exactly six hex digits and answers
 * FALSE (touching nothing) for anything else. */
gchar *oe_timeline_clip_color_hex (gint color_rgb);
gboolean oe_timeline_clip_color_parse_hex (const gchar *text, gint *color_rgb);

G_END_DECLS
