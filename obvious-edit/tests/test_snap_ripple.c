/* test_snap_ripple.c — Phase 7: pure snapping and the composite ripple
 * record. GTK-free like its siblings: the snap half talks straight to
 * OeSnapContext / oe_timeline_snap_time (the widget's preview path
 * builds the same context, so the decision is pinned at the seam); the
 * ripple half drives oe_edit_ripple_remove_clip + history replay over
 * real OeProject models, with JSON v1 saves as the sequence comparator
 * (same convention as test_undo_stack).
 *
 * Cases:
 *   /snap-ripple/snap-disabled         disabled context → raw candidate.
 *   /snap-ripple/snap-edges            neighbour edge targets, in/out of
 *                                      band, per candidate class (move
 *                                      position, trim source edge).
 *   /snap-ripple/snap-playhead         playhead target.
 *   /snap-ripple/snap-zero             zero target; negative raw.
 *   /snap-ripple/snap-frame-grid       frame boundaries at 25 fps; grid
 *                                      off at interval 0.
 *   /snap-ripple/snap-band-boundary    exactly at threshold snaps, one
 *                                      µs over does not.
 *   /snap-ripple/snap-tie-break        equidistant targets: earlier wins.
 *   /snap-ripple/snap-zoom-scaling     same delta, two zooms: band grows
 *                                      in µs as px_per_us shrinks.
 *   /snap-ripple/move-snap-then-clamp  snap feeds the legality clamp;
 *                                      a legal candidate passes through
 *                                      unchanged; adjacency stays legal.
 *   /snap-ripple/trim-snap-then-clamp  clamp wins when snap lands
 *                                      outside the trim bounds.
 *   /snap-ripple/ripple-first          ripple delete of the first clip:
 *                                      whole suffix shifts rigidly.
 *   /snap-ripple/ripple-middle         middle clip: suffix shifts, gap
 *                                      between suffix clips preserved.
 *   /snap-ripple/ripple-last           last clip: degenerates to a plain
 *                                      delete (empty shift list).
 *   /snap-ripple/ripple-rejection      bad indices: typed BAD_CLIP,
 *                                      nothing recorded.
 *   /snap-ripple/ripple-roundtrip      interleaved ops with ripples:
 *                                      full undo = pristine empty
 *                                      baseline; full redo = built
 *                                      baseline (JSON v1).
 *   /snap-ripple/ripple-depth          one composite record per action;
 *                                      101 pushes leave 100 records.
 *   /snap-ripple/ripple-redo-cleared   ripple → undo → edit: the redo
 *                                      branch is gone (typed error).
 *   /snap-ripple/ripple-auto-pause     record-time ripple while PLAYING
 *                                      does not pause (move/trim
 *                                      semantics); undo pauses first.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "../src/app/oe_playback_session.h"
#include "../src/app/oe_undo_stack.h"
#include "../src/core/oe_project.h"
#include "../src/core/oe_project_format.h"
#include "../src/playback/oe_audio_output.h"
#include "../src/ui/oe_timeline_layout.h"

#define US 1000000LL /* one second in µs, for readable bounds */

/* ------------------------------------------------------------------ */
/* Snap-core fixtures.                                                 */
/* ------------------------------------------------------------------ */

/* One video track, clips A [1 s, 2 s) and B [3 s, 4.5 s). */
static OeProject *
build_snap_fixture (void)
{
  OeProject *project = oe_project_new_default ();

  oe_project_add_media (project, "/media/snap.mp4");
  oe_project_add_track (project, OE_TRACK_VIDEO);

  g_assert_true (oe_project_insert_clip (project, 0, 1, 1 * US, 0, 1 * US, NULL));
  g_assert_true (oe_project_insert_clip (project, 0, 1, 3 * US, 0, 1500000, NULL));

  return project;
}

/* The same edge collection the widget does before building the
 * context: both edges of every clip on the track (the dragged clip is
 * excluded by the widget; here the caller passes the skip index). */
static GArray *
collect_edges (OeProject *project, guint track_index, guint skip_index)
{
  OeSequence sequence = { 0 }; /* zeroed storage per get_sequence contract */

  oe_project_get_sequence (project, &sequence);

  OeTrack *track = g_ptr_array_index (sequence.tracks, track_index);
  GArray *edges = g_array_new (FALSE, FALSE, sizeof (gint64));

  for (guint i = 0; i < track->clips->len; i++)
    {
      if (i == skip_index)
        continue;

      OeClip *clip = g_ptr_array_index (track->clips, i);
      gint64 duration = clip->source_out_us - clip->source_in_us;
      gint64 start = clip->position_us;
      gint64 end = clip->position_us + duration;

      g_array_append_vals (edges, (const gint64[]) { start }, 1);
      g_array_append_vals (edges, (const gint64[]) { end }, 1);
    }

  oe_sequence_clear (&sequence);
  return edges;
}

/* Context builder: enabled snapping, 8 px threshold, zoom in px/µs,
 * no playhead/grid unless the caller overrides. */
static OeSnapContext
snap_context (GArray *edges, gdouble px_per_us)
{
  OeSnapContext ctx;

  ctx.enabled = TRUE;
  ctx.threshold_px = OE_TIMELINE_SNAP_THRESHOLD_PX;
  ctx.px_per_us = px_per_us;
  ctx.playhead_us = -1; /* the tests opt in explicitly */
  ctx.frame_interval_us = 0;
  ctx.edges_us = edges->len > 0 ? (const gint64 *) edges->data : NULL;
  ctx.n_edges = edges->len;
  return ctx;
}

/* ------------------------------------------------------------------ */
/* Snap-core cases.                                                    */
/* ------------------------------------------------------------------ */

static void
test_snap_disabled (void)
{
  OeProject *project = build_snap_fixture ();
  GArray *edges = collect_edges (project, 0, G_MAXUINT);
  OeSnapContext ctx = snap_context (edges, 0.0001); /* 100 px/s */

  ctx.enabled = FALSE;

  /* A target dead-centre in any band changes nothing when disabled. */
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 2 * US + 10), ==, 2 * US + 10);
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 3 * US), ==, 3 * US);

  g_array_unref (edges);
  g_object_unref (project);
}

static void
test_snap_edges (void)
{
  OeProject *project = build_snap_fixture ();
  GArray *edges = collect_edges (project, 0, G_MAXUINT); /* 1s, 2s, 3s, 4.5s */
  OeSnapContext ctx = snap_context (edges, 0.0001);      /* 8 px = 80 ms band */

  /* A move candidate inside the band snaps onto the edge. */
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 2 * US + 50000), ==, 2 * US); /* in band */
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 3 * US - 50000), ==, 3 * US); /* in band */
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 2 * US + 500000), ==,
                   2 * US + 500000); /* outside */

  /* A trim-in candidate is the same decision over the same edge set:
   * the pointer time is what magnetizes, whichever drag it feeds. */
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 4500000 - 30000), ==, 4500000);       /* in band */
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 4 * US + 30000), ==, 4 * US + 30000); /* outside */

  g_array_unref (edges);
  g_object_unref (project);
}

static void
test_snap_playhead (void)
{
  OeProject *project = build_snap_fixture ();
  GArray *edges = collect_edges (project, 0, G_MAXUINT);
  OeSnapContext ctx = snap_context (edges, 0.0001); /* 80 ms band */

  ctx.playhead_us = 10 * US;

  g_assert_cmpint (oe_timeline_snap_time (&ctx, 10 * US + 79000), ==, 10 * US);
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 10 * US + 500000), ==, 10 * US + 500000);

  g_array_unref (edges);
  g_object_unref (project);
}

static void
test_snap_zero (void)
{
  OeProject *project = build_snap_fixture ();
  GArray *edges = collect_edges (project, 0, G_MAXUINT);
  OeSnapContext ctx = snap_context (edges, 0.0001); /* 80 ms band */

  g_assert_cmpint (oe_timeline_snap_time (&ctx, 79000), ==, 0);

  /* A negative candidate passes through untouched: the widget clamps
   * after, and the snap core never invents a positive position. */
  g_assert_cmpint (oe_timeline_snap_time (&ctx, -5), ==, -5);

  g_array_unref (edges);
  g_object_unref (project);
}

static void
test_snap_frame_grid (void)
{
  OeProject *project = build_snap_fixture ();
  GArray *edges = collect_edges (project, 0, G_MAXUINT);
  OeSnapContext ctx = snap_context (edges, 0.0001); /* 80 ms band */

  ctx.frame_interval_us = 40000; /* 25 fps */

  /* Mid-frame candidates snap to the nearest boundary. */
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 5 * US + 19000), ==, 5 * US);
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 5 * US + 25000), ==, 5 * US + 40000);

  /* Interval 0 disables the grid without touching other targets. */
  ctx.frame_interval_us = 0;
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 5 * US + 19), ==, 5 * US + 19);

  g_array_unref (edges);
  g_object_unref (project);
}

static void
test_snap_band_boundary (void)
{
  OeProject *project = build_snap_fixture ();
  GArray *edges = collect_edges (project, 0, G_MAXUINT);
  OeSnapContext ctx = snap_context (edges, 0.0001); /* 8 px → 80 ms */

  /* Exactly at the threshold snaps; one µs past it does not. The
   * band is inclusive: the edge of the band is still magnetic. */
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 2 * US + 80000), ==, 2 * US);
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 2 * US + 80001), ==, 2 * US + 80001);

  g_array_unref (edges);
  g_object_unref (project);
}

static void
test_snap_tie_break (void)
{
  OeProject *project = build_snap_fixture ();
  GArray *edges = collect_edges (project, 0, G_MAXUINT);
  OeSnapContext ctx = snap_context (edges, 0.0001); /* 80 ms band */

  /* Cross-class tie: the edge at 2 s and a frame boundary at
   * 2 s + 80 ms (grid = 80 ms) are both 40 ms from the candidate;
   * the earlier target (the edge) wins. */
  ctx.frame_interval_us = 80000;
  g_assert_cmpint (oe_timeline_snap_time (&ctx, 2 * US + 40000), ==, 2 * US);

  g_array_unref (edges);
  g_object_unref (project);
}

static void
test_snap_zoom_scaling (void)
{
  OeProject *project = build_snap_fixture ();
  GArray *edges = collect_edges (project, 0, G_MAXUINT);

  /* Zoomed in: 100 px/s → 80 ms band; a 200 ms miss is raw. */
  OeSnapContext zoomed_in = snap_context (edges, 0.0001);
  g_assert_cmpint (oe_timeline_snap_time (&zoomed_in, 2 * US + 200000), ==, 2 * US + 200000);

  /* Zoomed out: 10 px/s → 800 ms band; the SAME 200 ms delta now
   * snaps. Screen-space feels constant, time-space grows. */
  OeSnapContext zoomed_out = snap_context (edges, 0.00001);
  g_assert_cmpint (oe_timeline_snap_time (&zoomed_out, 2 * US + 200000), ==, 2 * US);

  g_array_unref (edges);
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */
/* Snap + clamp composition (the widget's preview pipeline).           */
/* ------------------------------------------------------------------ */

static void
test_snap_move_then_clamp (void)
{
  OeProject *project = build_snap_fixture ();
  GArray *edges = collect_edges (project, 0, 1);    /* dragging clip 1 (B) */
  OeSnapContext ctx = snap_context (edges, 0.0001); /* 80 ms band */
  OeSequence sequence = { 0 };                      /* zeroed storage per get_sequence contract */

  oe_project_get_sequence (project, &sequence);

  /* A legal in-band candidate snaps onto A's end and passes the clamp
   * unchanged: flush adjacency is legal. */
  gint64 wanted = oe_timeline_snap_time (&ctx, 2 * US + 50000);
  gint64 clamped = oe_timeline_clamp_move_position (&sequence, 0, 1, wanted);

  g_assert_cmpint (wanted, ==, 2 * US);
  g_assert_cmpint (clamped, ==, 2 * US);

  /* An off-band legal candidate keeps its raw position: the clamp no
   * longer fuses magnetism, so nothing pulls it to the neighbour. */
  wanted = oe_timeline_snap_time (&ctx, 2 * US + 500000);
  clamped = oe_timeline_clamp_move_position (&sequence, 0, 1, wanted);

  g_assert_cmpint (wanted, ==, 2 * US + 500000);
  g_assert_cmpint (clamped, ==, 2 * US + 500000);

  /* An infeasible candidate still recovers legally (clamp keeps its
   * job): B dragged onto A's middle recovers to contact. */
  clamped = oe_timeline_clamp_move_position (&sequence, 0, 1, 1500000);

  g_assert_cmpint (clamped, ==, 2 * US);

  oe_sequence_clear (&sequence);
  g_array_unref (edges);
  g_object_unref (project);
}

static void
test_snap_trim_then_clamp (void)
{
  OeProject *project = build_snap_fixture ();
  GArray *edges = collect_edges (project, 0, 1);    /* trimming clip 1 (B) */
  OeSnapContext ctx = snap_context (edges, 0.0001); /* 80 ms band */
  OeSequence sequence = { 0 };                      /* zeroed storage per get_sequence contract */

  oe_project_get_sequence (project, &sequence);

  OeClip clip;

  g_assert_true (oe_project_get_clip (project, 0, 1, &clip));

  gint64 min_in, max_in, min_out, max_out;

  oe_timeline_trim_bounds (&clip, 0, &min_in, &max_in, &min_out, &max_out);

  /* Trim-in: the snap magnetizes the candidate onto A's end (2 s),
   * but clip B owns only 1.5 s of source — the trim can never reach
   * that position, so the clamp wins: snap proposes, clamp disposes. */
  gint64 wanted = oe_timeline_snap_time (&ctx, 2 * US + 40000);
  gint64 preview = CLAMP (wanted, min_in, max_in);

  g_assert_cmpint (wanted, ==, 2 * US);
  g_assert_cmpint (preview, ==, clip.source_out_us - OE_TIMELINE_MIN_TRIM_US);

  /* The clamp stays authoritative on the out edge too: a candidate
   * under source_in + MIN_TRIM clamps back up regardless of the zero
   * target's magnetism. */
  wanted = oe_timeline_snap_time (&ctx, clip.source_in_us + OE_TIMELINE_MIN_TRIM_US - 100);
  preview = CLAMP (wanted, min_out, max_out);

  g_assert_cmpint (preview, ==, clip.source_in_us + OE_TIMELINE_MIN_TRIM_US);

  oe_sequence_clear (&sequence);
  g_array_unref (edges);
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */
/* Ripple fixtures: the undo-stack pattern with a ripple recorder.     */
/* ------------------------------------------------------------------ */

typedef struct
{
  OeProject *project;
  OeUndoStack *stack;
  guint track;
  guint ref_a;
  guint ref_b;
  guint ref_c;
} RippleFixture;

static void
ripple_fixture_setup (RippleFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = oe_project_new_default ();
  fx->stack = oe_undo_stack_new ();

  fx->track = oe_project_add_track (fx->project, OE_TRACK_VIDEO);
  fx->ref_a = oe_project_add_media (fx->project, "/fixtures/ripple-a.mp4");
  fx->ref_b = oe_project_add_media (fx->project, "/fixtures/ripple-b.mp4");
  fx->ref_c = oe_project_add_media (fx->project, "/fixtures/ripple-c.mp4");
}

static void
ripple_fixture_teardown (RippleFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  oe_undo_stack_free (fx->stack);
  g_object_unref (fx->project);
}

static OeClip
clip_make (guint media_ref, gint64 position_us, gint64 source_in_us, gint64 source_out_us)
{
  OeClip clip = { 0 };

  clip.media_ref = media_ref;
  clip.position_us = position_us;
  clip.source_in_us = source_in_us;
  clip.source_out_us = source_out_us;
  return clip;
}

static void
insert_ok (RippleFixture *fx, guint media_ref, gint64 position, gint64 in, gint64 out)
{
  GError *error = NULL;
  OeClip clip = clip_make (media_ref, position, in, out);

  g_assert_true (oe_edit_insert_clip (fx->project, fx->stack, fx->track, &clip, &error));
  g_assert_no_error (error);
}

/* Ripple-deletes and asserts the mutator accepted it. */
static void
ripple_ok (RippleFixture *fx, guint clip_index)
{
  GError *error = NULL;

  g_assert_true (
      oe_edit_ripple_remove_clip (fx->project, fx->stack, fx->track, clip_index, &error));
  g_assert_no_error (error);
}

static OeClip
clip_at (RippleFixture *fx, guint index)
{
  OeClip clip = { 0 };

  g_assert_true (oe_project_get_clip (fx->project, fx->track, index, &clip));
  return clip;
}

static gchar *
save_to_string (RippleFixture *fx, const gchar *basename)
{
  gchar *dir = g_dir_make_tmp ("oe-snap-ripple-XXXXXX", NULL);
  gchar *path = g_build_filename (dir, basename, NULL);
  GError *error = NULL;

  g_assert_true (oe_project_format_save (fx->project, path, &error));
  g_assert_no_error (error);

  gchar *contents = NULL;

  g_assert_true (g_file_get_contents (path, &contents, NULL, &error));
  g_assert_no_error (error);

  g_free (path);
  g_rmdir (dir);
  g_free (dir);
  return contents;
}

/* Asserts the whole track footprint: (position, duration, media) per
 * clip, ordered by index. */
static void
assert_track_shape (RippleFixture *fx, guint n, const gint64 (*shape)[3])
{
  g_assert_cmpuint (oe_project_get_clip_count (fx->project, fx->track), ==, n);

  for (guint i = 0; i < n; i++)
    {
      OeClip clip = clip_at (fx, i);

      g_assert_cmpint (clip.position_us, ==, shape[i][0]);
      g_assert_cmpint (clip.source_out_us - clip.source_in_us, ==, shape[i][1]);
      g_assert_cmpuint (clip.media_ref, ==, (guint) shape[i][2]);
    }
}

/* ------------------------------------------------------------------ */
/* Ripple cases.                                                       */
/* ------------------------------------------------------------------ */

/* Three clips with gaps: A [0,5) B [8,11) C [15,17). Deleting A moves
 * B and C left by 5 s, rigidly: the B→C gap is preserved. One record;
 * undo restores the exact pre-state; redo reproduces the post-state. */
static void
test_ripple_first (RippleFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_a, 0, 0, 5 * US);
  insert_ok (fx, fx->ref_b, 8 * US, 0, 3 * US);
  insert_ok (fx, fx->ref_c, 15 * US, 0, 2 * US);

  gchar *baseline = save_to_string (fx, "base.oe");
  const guint size_before = oe_undo_stack_get_size (fx->stack);

  ripple_ok (fx, 0);

  const gint64 after[][3] = {
    { 3 * US, 3 * US, (gint64) fx->ref_b },
    { 10 * US, 2 * US, (gint64) fx->ref_c },
  };

  assert_track_shape (fx, 2, after);

  /* Exactly one composite record for the whole action. */
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, size_before + 1);
  g_assert_true (oe_undo_stack_can_undo (fx->stack));
  g_assert_false (oe_undo_stack_can_redo (fx->stack));

  const OeUndoRecord *record = NULL;
  GError *error = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (record->kind, ==, OE_UNDO_OP_RIPPLE_DELETE);
  g_assert_cmpstr (record->label, ==, "Ripple delete clip 0 on track 0");
  g_assert_nonnull (record->ripple_shifts);
  g_assert_cmpuint (record->ripple_shifts->len, ==, 2);

  /* Pre identities recorded across the renumbering: suffix clip k had
   * pre_index 1 + k and post_index k. */
  const OeRippleShift *shift_b = &g_array_index (record->ripple_shifts, OeRippleShift, 0);
  const OeRippleShift *shift_c = &g_array_index (record->ripple_shifts, OeRippleShift, 1);

  g_assert_cmpuint (shift_b->pre_index, ==, 1);
  g_assert_cmpuint (shift_b->post_index, ==, 0);
  g_assert_cmpint (shift_b->pre_position_us, ==, 8 * US);
  g_assert_cmpint (shift_b->post_position_us, ==, 3 * US);
  g_assert_cmpuint (shift_c->pre_index, ==, 2);
  g_assert_cmpuint (shift_c->post_index, ==, 1);
  g_assert_cmpint (shift_c->pre_position_us, ==, 15 * US);
  g_assert_cmpint (shift_c->post_position_us, ==, 10 * US);

  /* Undo restores the exact pre-state — byte-identical through v1. */
  gchar *undone = save_to_string (fx, "undone.oe");

  g_assert_cmpstr (undone, ==, baseline);
  g_free (undone);

  g_assert_true (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);

  const gint64 redone[][3] = {
    { 3 * US, 3 * US, (gint64) fx->ref_b },
    { 10 * US, 2 * US, (gint64) fx->ref_c },
  };

  assert_track_shape (fx, 2, redone);

  g_free (baseline);
}

/* Middle delete: A [0,5) B [8,11) C [15,17); deleting B shifts only C
 * left by 3 s. Undo in one step; redo in one step. */
static void
test_ripple_middle (RippleFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_a, 0, 0, 5 * US);
  insert_ok (fx, fx->ref_b, 8 * US, 0, 3 * US);
  insert_ok (fx, fx->ref_c, 15 * US, 0, 2 * US);

  const guint size_before = oe_undo_stack_get_size (fx->stack);

  ripple_ok (fx, 1);

  const gint64 after[][3] = {
    { 0, 5 * US, (gint64) fx->ref_a },
    { 12 * US, 2 * US, (gint64) fx->ref_c },
  };

  assert_track_shape (fx, 2, after);
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, size_before + 1);

  const OeUndoRecord *record = NULL;
  GError *error = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);

  const gint64 restored[][3] = {
    { 0, 5 * US, (gint64) fx->ref_a },
    { 8 * US, 3 * US, (gint64) fx->ref_b },
    { 15 * US, 2 * US, (gint64) fx->ref_c },
  };

  assert_track_shape (fx, 3, restored);

  g_assert_true (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  assert_track_shape (fx, 2, after);
}

/* Last delete: no downstream clips, the shift list is empty and the
 * record degenerates to the plain delete payload. */
static void
test_ripple_last (RippleFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_a, 0, 0, 5 * US);
  insert_ok (fx, fx->ref_b, 8 * US, 0, 3 * US);

  const guint size_before = oe_undo_stack_get_size (fx->stack);

  ripple_ok (fx, 1);

  const gint64 after[][3] = {
    { 0, 5 * US, (gint64) fx->ref_a },
  };

  assert_track_shape (fx, 1, after);
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, size_before + 1);

  const OeUndoRecord *record = NULL;
  GError *error = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_nonnull (record->ripple_shifts);
  g_assert_cmpuint (record->ripple_shifts->len, ==, 0);

  const gint64 restored[][3] = {
    { 0, 5 * US, (gint64) fx->ref_a },
    { 8 * US, 3 * US, (gint64) fx->ref_b },
  };

  assert_track_shape (fx, 2, restored);

  g_assert_true (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  assert_track_shape (fx, 1, after);
}

/* Bad indices reject with the model's typed error before any
 * mutation; nothing is recorded. */
static void
test_ripple_rejection (RippleFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_a, 0, 0, 5 * US);

  GError *error = NULL;
  const guint size_before = oe_undo_stack_get_size (fx->stack);

  g_assert_false (oe_edit_ripple_remove_clip (fx->project, fx->stack, fx->track, 9, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP);
  g_clear_error (&error);

  /* The rejection records nothing: the stack keeps only the setup
   * insert, and the model is untouched. */
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, size_before);
  g_assert_cmpuint (oe_project_get_clip_count (fx->project, fx->track), ==, 1);
}

/* Interleaved ripple + move + trim + insert history: full reverse undo
 * lands on a pristine empty baseline, full redo rebuilds the baseline,
 * byte-identical through the v1 serializer. */
static void
test_ripple_roundtrip (RippleFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  /* Forward build (interleaved kinds). */
  insert_ok (fx, fx->ref_a, 0, 0, 5 * US);       /* index 0 */
  insert_ok (fx, fx->ref_b, 8 * US, 0, 3 * US);  /* index 1 */
  insert_ok (fx, fx->ref_c, 15 * US, 0, 2 * US); /* index 2 */

  GError *error = NULL;

  g_assert_true (oe_edit_move_clip (fx->project, fx->stack, fx->track, 2, 16 * US, &error));
  g_assert_no_error (error);
  g_assert_true (oe_edit_trim_clip (fx->project, fx->stack, fx->track, 1, 0, 2 * US, &error));
  g_assert_no_error (error);
  ripple_ok (fx, 0); /* ripple delete A: B and C shift left 5 s */

  gchar *baseline = save_to_string (fx, "base.oe");
  const guint record_count = oe_undo_stack_get_size (fx->stack);

  g_assert_cmpuint (record_count, ==, 6); /* 3 inserts + move + trim + ripple */

  /* Pristine twin minus every clip: media and tracks are
   * history-independent. */
  OeProject *pristine = oe_project_new_default ();

  g_assert_cmpuint (oe_project_add_track (pristine, OE_TRACK_VIDEO), ==, fx->track);
  oe_project_add_media (pristine, "/fixtures/ripple-a.mp4");
  oe_project_add_media (pristine, "/fixtures/ripple-b.mp4");
  oe_project_add_media (pristine, "/fixtures/ripple-c.mp4");

  RippleFixture pristine_fx = { pristine, NULL, fx->track, 0, 0, 0 };
  gchar *empty_saved_baseline = save_to_string (&pristine_fx, "empty.oe");

  g_object_unref (pristine);

  /* Full reverse undo: lands byte-identical on the empty baseline. */
  for (guint i = 0; i < record_count; i++)
    {
      const OeUndoRecord *record = NULL;

      g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
      g_assert_no_error (error);
    }
  g_assert_false (oe_undo_stack_can_undo (fx->stack));
  g_assert_true (oe_undo_stack_can_redo (fx->stack));

  gchar *undone = save_to_string (fx, "undone.oe");

  g_assert_cmpstr (undone, ==, empty_saved_baseline);
  g_free (undone);

  /* Full redo: the built baseline comes back exactly. */
  for (guint i = 0; i < record_count; i++)
    {
      const OeUndoRecord *record = NULL;

      g_assert_true (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
      g_assert_no_error (error);
    }
  g_assert_true (oe_undo_stack_can_undo (fx->stack));
  g_assert_false (oe_undo_stack_can_redo (fx->stack));

  gchar *redone = save_to_string (fx, "redone.oe");

  g_assert_cmpstr (redone, ==, baseline);
  g_free (redone);

  g_free (baseline);
  g_free (empty_saved_baseline);
}

/* One composite record per action: 101 ripple deletes leave exactly
 * 100 records (the oldest is evicted, never a sub-step). */
static void
test_ripple_depth (RippleFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  for (guint i = 0; i < OE_UNDO_STACK_MAX_DEPTH + 1; i++)
    {
      OeClip clip = clip_make (fx->ref_a, 0, 0, 1 * US);

      g_assert_true (oe_edit_insert_clip (fx->project, fx->stack, fx->track, &clip, &error));
      g_assert_no_error (error);

      ripple_ok (fx, 0);
    }

  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, OE_UNDO_STACK_MAX_DEPTH);
}

/* Ripple → undo → record a fresh edit: the redo branch is gone and
 * redo fails with the typed EMPTY error. */
static void
test_ripple_redo_cleared (RippleFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_a, 0, 0, 5 * US);
  insert_ok (fx, fx->ref_b, 8 * US, 0, 3 * US);

  ripple_ok (fx, 0);

  const OeUndoRecord *record = NULL;
  GError *error = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_true (oe_undo_stack_can_redo (fx->stack));

  /* A fresh edit on top of the undone state discards the redo
   * branch — composite records obey the same linear-history rule. */
  insert_ok (fx, fx->ref_c, 30 * US, 0, 2 * US);

  g_assert_false (oe_undo_stack_can_redo (fx->stack));
  g_assert_false (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_error (error, OE_UNDO_STACK_ERROR, OE_UNDO_STACK_ERROR_EMPTY);
  g_clear_error (&error);
}

/* Virtual clock: record-time ripple while PLAYING does not pause
 * (move/trim semantics — the deep-copy staleness contract covers the
 * playing copy); undoing the composite record through the session
 * variant pauses FIRST, then applies. */
static gint64 fake_time_us = 0;

static gint64
read_fake_time (gpointer user_data G_GNUC_UNUSED)
{
  return fake_time_us;
}

static void
test_ripple_auto_pause (RippleFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_a, 0, 0, 5 * US);
  insert_ok (fx, fx->ref_b, 8 * US, 0, 3 * US);

  OePlaybackSession *session = oe_playback_session_new (fx->project);

  oe_playback_session_set_time_source (session, read_fake_time, NULL);

  GError *error = NULL;

  g_assert_true (oe_playback_session_play (session, &error));
  g_assert_no_error (error);
  g_assert_cmpint (oe_playback_session_get_state (session), ==, OE_PLAYBACK_PLAYING);

  /* Record path: mutates the model, never touches the session. */
  ripple_ok (fx, 0);
  g_assert_cmpint (oe_playback_session_get_state (session), ==, OE_PLAYBACK_PLAYING);

  /* History path: pause first, then apply the composite inverse. */
  fake_time_us += 2 * US;

  const OeUndoRecord *record = NULL;

  g_assert_true (
      oe_undo_stack_undo_with_session (fx->stack, fx->project, session, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpint (oe_playback_session_get_state (session), !=, OE_PLAYBACK_PLAYING);

  g_assert_true (
      oe_undo_stack_redo_with_session (fx->stack, fx->project, session, &record, &error));
  g_assert_no_error (error);

  oe_playback_session_free (session);
}

/* ------------------------------------------------------------------ */
/* Suite.                                                              */
/* ------------------------------------------------------------------ */

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  /* The auto-pause test plays a session: init the audio adapter for
   * the whole run (dummy driver via the meson test environment). */
  GError *audio_error = NULL;

  if (!oe_audio_output_init (&audio_error))
    {
      g_printerr ("audio init failed: %s\n", audio_error->message);
      g_error_free (audio_error);
      return 1;
    }

  g_test_add_func ("/snap-ripple/snap-disabled", test_snap_disabled);
  g_test_add_func ("/snap-ripple/snap-edges", test_snap_edges);
  g_test_add_func ("/snap-ripple/snap-playhead", test_snap_playhead);
  g_test_add_func ("/snap-ripple/snap-zero", test_snap_zero);
  g_test_add_func ("/snap-ripple/snap-frame-grid", test_snap_frame_grid);
  g_test_add_func ("/snap-ripple/snap-band-boundary", test_snap_band_boundary);
  g_test_add_func ("/snap-ripple/snap-tie-break", test_snap_tie_break);
  g_test_add_func ("/snap-ripple/snap-zoom-scaling", test_snap_zoom_scaling);
  g_test_add_func ("/snap-ripple/move-snap-then-clamp", test_snap_move_then_clamp);
  g_test_add_func ("/snap-ripple/trim-snap-then-clamp", test_snap_trim_then_clamp);

#define ADD_RIPPLE_TEST(path, func)                                                                \
  g_test_add ((path), RippleFixture, NULL, ripple_fixture_setup, (func), ripple_fixture_teardown)

  ADD_RIPPLE_TEST ("/snap-ripple/ripple-first", test_ripple_first);
  ADD_RIPPLE_TEST ("/snap-ripple/ripple-middle", test_ripple_middle);
  ADD_RIPPLE_TEST ("/snap-ripple/ripple-last", test_ripple_last);
  ADD_RIPPLE_TEST ("/snap-ripple/ripple-rejection", test_ripple_rejection);
  ADD_RIPPLE_TEST ("/snap-ripple/ripple-roundtrip", test_ripple_roundtrip);
  ADD_RIPPLE_TEST ("/snap-ripple/ripple-depth", test_ripple_depth);
  ADD_RIPPLE_TEST ("/snap-ripple/ripple-redo-cleared", test_ripple_redo_cleared);
  ADD_RIPPLE_TEST ("/snap-ripple/ripple-auto-pause", test_ripple_auto_pause);

#undef ADD_RIPPLE_TEST

  const int result = g_test_run ();

  oe_audio_output_shutdown ();
  return result;
}
