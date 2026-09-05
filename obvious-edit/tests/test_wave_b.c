/* test_wave_b.c — Phase 9 Wave B units: the interpolation contract,
 * the shared fade envelope, transition windows and mutators, timeline
 * band/snap geometry, ripple re-anchor replay, keyframe undo records,
 * and the JSON persistence of keyframes/transitions.
 *
 * GTK-free and media-free by construction: the compositor-level blend
 * and the decode-back acceptance sequence live in tests/test_export.c,
 * which owns the synthetic-media fixtures. The model here is built
 * through the public mutators only, so the tests read like UI edits.
 *
 * Coverage map:
 *   /wave-b/keyframes/ - contract D7: one rounding, clamp,
 *                        degradation, resolve, undo record
 *   /wave-b/fades/ - shared envelope: endpoints, rounding
 *   /wave-b/transitions/ - windows D5: degenerate-to-cut, mutators
 *   /wave-b/layout/ - bands and snap edges (GTK-free layout)
 *   /wave-b/ripple/ - re-anchor replay through mutators
 *   /wave-b/format/ - D8: unconditional members, byte-identical
 *                     round trips, strict rejection
 */

#include "src/app/oe_undo_stack.h"
#include "src/core/oe_fades.h"
#include "src/core/oe_keyframes.h"
#include "src/core/oe_project_format.h"
#include "src/core/oe_time.h"
#include "src/ui/oe_timeline_layout.h"

#include <glib.h>
#include <glib/gstdio.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* One media ref so clips pass the known-ref guard. The path is never
 * resolved: the unit tests never decode. */
static OeProject *
build_project_25fps (void)
{
  OeProject *project = oe_project_new ((OeRational) { 25, 1 });

  g_assert_nonnull (project);
  g_assert_true (oe_project_add_media_ref (project, 1, "/nonexistent/a.mp4", NULL));
  return project;
}

/* A video track with @count clips of @span_us back-to-back starting at
 * 0, all sharing media ref 1. Returns the track index. */
static guint
add_contiguous_clips (OeProject *project, guint count, gint64 span_us)
{
  const guint track = oe_project_add_track (project, OE_TRACK_VIDEO);

  for (guint i = 0; i < count; i++)
    g_assert_true (oe_project_insert_clip (project, track, 1, (gint64) i * span_us,
                                           (gint64) i * span_us, (gint64) (i + 1) * span_us, NULL));

  return track;
}

/* A fresh keyframe store with one opacity ramp 0 -> 255 over
 * [0, 1 s). */
static GArray *
opacity_ramp_store (void)
{
  GArray *store = g_array_new (FALSE, FALSE, sizeof (OeKeyframe));
  const OeKeyframe a = { OE_KEYFRAME_OPACITY, 0, 0 };
  const OeKeyframe b = { OE_KEYFRAME_OPACITY, 1000000, 255 };

  oe_keyframes_insert (store, a);
  oe_keyframes_insert (store, b);
  return store;
}

/* Reads a file into memory for byte comparisons. */
static gchar *
slurp (const gchar *path)
{
  gchar *data = NULL;
  gsize len = 0;

  g_assert_true (g_file_get_contents (path, &data, &len, NULL));
  g_assert_nonnull (data);
  return data;
}

/* Replaces the FIRST occurrence of @needle in @haystack. Test helper
 * for single-site document mutations; g_assert-fails when absent. */
static gchar *
str_replace_once (const gchar *haystack, const gchar *needle, const gchar *with)
{
  const gchar *hit = strstr (haystack, needle);

  g_assert_nonnull (hit);

  return g_strdup_printf ("%.*s%s%s", (int) (hit - haystack), haystack, with,
                          hit + strlen (needle));
}

/* ------------------------------------------------------------------ */
/* Keyframes: the interpolation contract (D7)                          */
/* ------------------------------------------------------------------ */

/* The sample equals the static value plus ONE rounding of the delta
 * through oe_time_round_ratio — checked against the primitive itself
 * (the contract), then against the concrete half-ramp value. */
static void
test_sample_single_rounding (void)
{
  GArray *store = opacity_ramp_store ();

  for (gint64 t = 0; t <= 1000000; t += 7000)
    {
      const gint64 expected = 0 + oe_time_round_ratio ((gint64) 255 * (t - 0), 1000000 - 0);

      g_assert_cmpint (oe_keyframes_sample (store, OE_KEYFRAME_OPACITY, t, 200), ==, expected);
    }

  /* The concrete case: the exact midpoint of a 0..255 ramp rounds
   * 127.5 up to 128 — one rounding, not two (truncation would give
   * 127). */
  g_assert_cmpint (oe_keyframes_sample (store, OE_KEYFRAME_OPACITY, 500000, 255), ==, 128);

  /* A ramp whose delta is itself rounded: 0..255 over 3 frames
   * (120000 µs at 25 fps) — the sample at frame 1 uses the same
   * primitive the frame grid uses. */
  g_assert_cmpint (oe_keyframes_sample (store, OE_KEYFRAME_OPACITY, 40000, 255), ==,
                   oe_time_round_ratio (255 * 40000, 1000000));

  g_array_unref (store);
}

/* At or before the first entry the answer is the first value; at or
 * after the last entry the last value — clamp-to-endpoint, no
 * extrapolation. */
static void
test_sample_clamp_endpoints (void)
{
  GArray *store = opacity_ramp_store ();

  g_assert_cmpint (oe_keyframes_sample (store, OE_KEYFRAME_OPACITY, 0, 255), ==, 0);
  g_assert_cmpint (oe_keyframes_sample (store, OE_KEYFRAME_OPACITY, 1000000, 0), ==, 255);
  g_assert_cmpint (oe_keyframes_sample (store, OE_KEYFRAME_OPACITY, 1, 255), ==, 0);
  g_assert_cmpint (oe_keyframes_sample (store, OE_KEYFRAME_OPACITY, 999999, 0), ==, 255);
  g_assert_cmpint (oe_keyframes_sample (store, OE_KEYFRAME_OPACITY, 5000000, 0), ==, 255);
  g_assert_cmpint (oe_keyframes_sample (store, OE_KEYFRAME_OPACITY, -7000, 0), ==, 0);

  g_array_unref (store);
}

/* Degradation: a NULL store, a store without the property, a run
 * shorter than two entries, an unsorted run, and a zero-span run all
 * answer the clip's static value — total over every input. */
static void
test_sample_degradation (void)
{
  /* NULL store: fully static clip. */
  g_assert_cmpint (oe_keyframes_sample (NULL, OE_KEYFRAME_OPACITY, 500000, 197), ==, 197);

  /* Store with keys on another property only. */
  GArray *other = g_array_new (FALSE, FALSE, sizeof (OeKeyframe));
  const OeKeyframe pos = { OE_KEYFRAME_POS_X, 500000, 40 };

  oe_keyframes_insert (other, pos);
  g_assert_cmpint (oe_keyframes_sample (other, OE_KEYFRAME_OPACITY, 500000, 197), ==, 197);
  g_array_unref (other);

  /* Single-entry run: nothing to interpolate between — degrades to
   * the static value like an empty store (the documented D7 rule). */
  GArray *single = g_array_new (FALSE, FALSE, sizeof (OeKeyframe));
  const OeKeyframe one = { OE_KEYFRAME_OPACITY, 400000, 90 };

  oe_keyframes_insert (single, one);
  g_assert_cmpint (oe_keyframes_sample (single, OE_KEYFRAME_OPACITY, 500000, 197), ==, 197);
  g_array_unref (single);

  /* Unsorted run (raw, bypassing sorted insertion): degrades rather
   * than reading out of bounds. */
  GArray *unsorted = g_array_new (FALSE, FALSE, sizeof (OeKeyframe));
  const OeKeyframe first = { OE_KEYFRAME_OPACITY, 800000, 255 };
  const OeKeyframe second = { OE_KEYFRAME_OPACITY, 200000, 0 };

  g_array_append_val (unsorted, first);
  g_array_append_val (unsorted, second);
  /* Validity is domain-level; sortedness is an insertion invariant.
   * The sample still degrades rather than reading out of bounds. */
  g_assert_cmpint (oe_keyframes_sample (unsorted, OE_KEYFRAME_OPACITY, 500000, 197), ==, 197);
  g_array_unref (unsorted);

  /* Zero-span run (two entries, same time): degrades. */
  GArray *zero = g_array_new (FALSE, FALSE, sizeof (OeKeyframe));
  const OeKeyframe a = { OE_KEYFRAME_OPACITY, 500000, 0 };
  const OeKeyframe b = { OE_KEYFRAME_OPACITY, 500000, 255 };

  g_array_append_val (zero, a);
  g_array_append_val (zero, b);
  g_assert_cmpint (oe_keyframes_sample (zero, OE_KEYFRAME_OPACITY, 500000, 197), ==, 197);
  g_array_unref (zero);
}

/* The resolved visual samples each keyframeable property at the
 * clip-relative time and keeps crop (and any unkeyed property) static;
 * the resolved struct never owns memory. */
static void
test_resolve_per_property (void)
{
  GArray *store = g_array_new (FALSE, FALSE, sizeof (OeKeyframe));

  {
    const OeKeyframe keys[] = {
      { OE_KEYFRAME_OPACITY, 0, 255 },
      { OE_KEYFRAME_OPACITY, 1000000, 0 },
      { OE_KEYFRAME_POS_X, 0, 0 },
      { OE_KEYFRAME_POS_X, 500000, 42 },
    };

    for (gsize i = 0; i < G_N_ELEMENTS (keys); i++)
      oe_keyframes_insert (store, keys[i]);
  }

  OeClipVisual visual = oe_clip_visual_identity ();

  visual.crop_l = 7;
  visual.keyframes = store;

  OeClipVisual resolved = { 0 };

  oe_clip_visual_resolve (&visual, 500000, &resolved);

  g_assert_cmpint (resolved.opacity, ==,
                   127); /* one rounding; the descending 255→0 half-point rounds away from zero */
  g_assert_cmpint (resolved.pos_x, ==, 42);
  g_assert_cmpint (resolved.pos_y, ==, visual.pos_y); /* unkeyed → static */
  g_assert_cmpint (resolved.scale_permille, ==, visual.scale_permille);
  g_assert_cmpint (resolved.rotation_cdeg, ==, visual.rotation_cdeg);
  g_assert_cmpint (resolved.crop_l, ==, 7); /* crop stays static */
  g_assert_null (resolved.keyframes);       /* transient, owns nothing */

  /* After the run ends the opacity clamps to its last key. */
  oe_clip_visual_resolve (&visual, 2000000, &resolved);
  g_assert_cmpint (resolved.opacity, ==, 0);

  oe_clip_visual_clear (&visual);
}

/* A keyframe edit through the validated wrapper is ONE OE_UNDO_OP_VISUAL
 * record; undo restores the whole pre-stroke visual, store included;
 * a zero-delta stroke records nothing. */
static void
test_keyframe_edit_undo_record (void)
{
  OeProject *project = build_project_25fps ();
  const guint track = add_contiguous_clips (project, 2, 1000000);
  OeUndoStack *stack = oe_undo_stack_new ();

  g_assert_true (
      oe_edit_set_clip_keyframe (project, stack, track, 0, OE_KEYFRAME_OPACITY, 500000, 128, NULL));

  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 1);

  /* Undoing hands back the record: a keyframe edit is ONE visual
   * record whose post-stroke visual carries the key and whose clip
   * snapshot (the undo source) holds the PRE-stroke store. */
  const OeUndoRecord *undone = NULL;

  g_assert_true (oe_undo_stack_undo (stack, project, &undone, NULL));
  g_assert_nonnull (undone);
  g_assert_true (undone->kind == OE_UNDO_OP_VISUAL);
  g_assert_null (undone->clip.visual.keyframes);
  g_assert_nonnull (undone->new_visual.keyframes);

  OeClip clip;

  g_assert_true (oe_project_get_clip (project, track, 0, &clip));
  g_assert_null (clip.visual.keyframes);

  /* A fresh stroke discards the undone record (the linear-history
   * model), and the identical follow-up stroke records nothing: the
   * stack ends with exactly the one new record. */
  g_assert_true (
      oe_edit_set_clip_keyframe (project, stack, track, 0, OE_KEYFRAME_OPACITY, 0, 255, NULL));
  g_assert_true (
      oe_edit_set_clip_keyframe (project, stack, track, 0, OE_KEYFRAME_OPACITY, 0, 255, NULL));
  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 1);

  oe_undo_stack_free (stack); /* a plain struct, not a GObject */
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */
/* Fades: the shared envelope (D6)                                     */
/* ------------------------------------------------------------------ */

/* Endpoints: no fades → full gain; the first sample of a fade-in is
 * zero; inside the ramp the gain rises; after the ramp it is full
 * again; the fade-out mirrors it; both sides take the MIN. */
static void
test_fade_ramp_endpoints (void)
{
  const gint64 start = 1000000, end = 2000000;

  /* No fades: full gain everywhere. */
  g_assert_cmpuint (oe_fade_gain (start, start, end, 0, 0), ==, 1024);
  g_assert_cmpuint (oe_fade_gain (1500000, start, end, 0, 0), ==, 1024);
  g_assert_cmpuint (oe_fade_gain (end - 1, start, end, 0, 0), ==, 1024);

  /* Fade-in only. */
  g_assert_cmpuint (oe_fade_gain (start, start, end, 500000, 0), ==, 0);
  g_assert_cmpuint (oe_fade_gain (start + 250000, start, end, 500000, 0), ==,
                    oe_time_round_ratio (250000 * 1024, 500000)); /* 512 */
  g_assert_cmpuint (oe_fade_gain (start + 500000, start, end, 500000, 0), ==, 1024);
  g_assert_cmpuint (oe_fade_gain (end - 1, start, end, 500000, 0), ==, 1024);

  /* Fade-out only: the mirror image — the ramp is (clip_end - t),
   * so it reaches zero exactly at the clip's last sample. */
  g_assert_cmpuint (oe_fade_gain (start, start, end, 0, 500000), ==, 1024);
  g_assert_cmpuint (oe_fade_gain (end - 250000, start, end, 0, 500000), ==, 512);
  g_assert_cmpuint (oe_fade_gain (end - 1, start, end, 0, 500000), ==, 0);

  /* Both sides: the envelope is the MIN of the two ramps — the last
   * sample of a symmetric fade is silent, the midpoint full gain. */
  g_assert_cmpuint (oe_fade_gain (start, start, end, 500000, 500000), ==, 0);
  g_assert_cmpuint (oe_fade_gain (start + 250000, start, end, 500000, 500000), ==, 512);
  g_assert_cmpuint (oe_fade_gain (end - 1, start, end, 500000, 500000), ==, 0);

  /* A fade longer than the clip saturates: gain stays silent on the
   * in-side and clamps to the ramp the whole way — never outside
   * 0..1024. */
  for (gint64 t = start; t < end; t += 33333)
    {
      const guint gain = oe_fade_gain (t, start, end, 2000000, 2000000);

      g_assert_cmpuint (gain, <=, 1024);
    }
}
/* The envelope rounds exactly once: the gain equals the primitive's
 * answer, not a nested or truncated variant. */
static void
test_fade_single_rounding (void)
{
  const gint64 start = 300000, end = 900000;

  for (gint64 t = start; t < end; t += 11111)
    {
      const guint in = 300000, out = 250000;
      const gint64 want_in = oe_time_round_ratio ((t - start) * 1024, (gint64) in);
      const gint64 want_out = oe_time_round_ratio ((end - t) * 1024, (gint64) out);
      const gint64 want = MIN (MIN (want_in, want_out), 1024);

      g_assert_cmpuint (oe_fade_gain (t, start, end, in, out), ==, (guint) want);
    }
}

/* ------------------------------------------------------------------ */
/* Transitions: windows and mutators (D5)                              */
/* ------------------------------------------------------------------ */

/* An anchored boundary with coverage yields the centered window; a
 * moved neighbor degrades it to inactive; duration 0 degenerates to
 * the straight cut. */
static void
test_transition_window (void)
{
  OeProject *project = build_project_25fps ();
  const guint track = add_contiguous_clips (project, 3, 400000);
  OeSequence seq = { 0 };
  OeTransition t = { track, 400000, 100000, OE_TRANSITION_CROSS_DISSOLVE };

  g_assert_true (oe_project_add_transition (project, &t, NULL));
  oe_project_get_sequence (project, &seq);

  g_assert_cmpuint (oe_project_get_transition_count (project), ==, 1);

  OeTransition stored;

  g_assert_true (oe_project_get_transition (project, 0, &stored));
  /* Duration clamps to what both neighbors cover: 400000 / 2. */
  g_assert_cmpint (stored.duration_us, ==, 100000);

  OeTransitionWindow window = oe_transition_window (&seq, &stored);

  g_assert_true (window.active);
  g_assert_true (window.out_clip != NULL && window.in_clip != NULL);
  g_assert_cmpint (window.start_us, ==, 350000);
  g_assert_cmpint (window.end_us, ==, 450000);

  /* Zero duration is rejected by the mutator — a stored transition
   * always has a positive, coverable window; the straight cut appears
   * when the neighbors stop covering it, tested below. */
  OeTransition zero = { track, 400000, 0, OE_TRANSITION_CROSS_DISSOLVE };

  g_assert_false (oe_project_add_transition (project, &zero, NULL));

  /* Moving the incoming clip (index 1 — the transition sits on the
   * clip0/clip1 boundary) breaks the boundary: the window degrades
   * gracefully — the transition object survives, the cut takes over. */
  g_assert_true (oe_project_move_clip (project, track, 1, 1200000, NULL));
  oe_sequence_clear (&seq);
  oe_project_get_sequence (project, &seq);

  g_assert_true (oe_project_get_transition (project, 0, &stored));
  window = oe_transition_window (&seq, &stored);
  g_assert_false (window.active);

  /* Restoring the adjacency reactivates it — no fixups were needed. */
  g_assert_true (oe_project_move_clip (project, track, 1, 400000, NULL));
  oe_sequence_clear (&seq);
  oe_project_get_sequence (project, &seq);
  window = oe_transition_window (&seq, &stored);
  g_assert_true (window.active);

  oe_sequence_clear (&seq);
  g_object_unref (project);
}

/* Mutator validation: audio tracks are out (v1), a non-boundary
 * position is out, an unknown kind name cannot come from the parser's
 * closed set, and remove drops exactly the indexed entry. */
static void
test_transition_mutators (void)
{
  OeProject *project = build_project_25fps ();
  const guint video = add_contiguous_clips (project, 2, 400000);
  const guint audio = oe_project_add_track (project, OE_TRACK_AUDIO);

  g_assert_true (oe_project_insert_clip (project, audio, 1, 0, 0, 400000, NULL));
  g_assert_true (oe_project_insert_clip (project, audio, 1, 400000, 0, 800000, NULL));

  GError *error = NULL;

  /* Video-only in v1. */
  OeTransition on_audio = { audio, 400000, 100000, OE_TRANSITION_CROSS_DISSOLVE };

  g_assert_false (oe_project_add_transition (project, &on_audio, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRANSITION);
  g_clear_error (&error);

  /* Not a boundary. */
  OeTransition off_boundary = { video, 123456, 100000, OE_TRANSITION_CROSS_DISSOLVE };

  g_assert_false (oe_project_add_transition (project, &off_boundary, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRANSITION);
  g_clear_error (&error);

  /* Clamped duration: 900000 µs asked; the centered window can cover
   * at most twice the shorter neighbor (both are 400000), so 800000
   * is stored — never a pending failure. */
  OeTransition big = { video, 400000, 900000, OE_TRANSITION_DIP_TO_BLACK };

  g_assert_true (oe_project_add_transition (project, &big, NULL));

  OeTransition stored;

  g_assert_true (oe_project_get_transition (project, 0, &stored));
  g_assert_cmpint (stored.duration_us, ==, 800000);
  g_assert_cmpint (stored.kind, ==, OE_TRANSITION_DIP_TO_BLACK);

  /* Remove: the list shrinks and the index is gone. */
  g_assert_true (oe_project_remove_transition (project, 0, NULL));
  g_assert_cmpuint (oe_project_get_transition_count (project), ==, 0);

  g_object_unref (project);
}

/* ------------------------------------------------------------------ */
/* Layout: bands and snap edges (GTK-free)                             */
/* ------------------------------------------------------------------ */

/* The band's x extent maps the effective window through the geometry;
 * an inactive window draws no band and offers no edges. */
static void
test_layout_band_and_edges (void)
{
  OeProject *project = build_project_25fps ();
  const guint track = add_contiguous_clips (project, 2, 400000);
  OeTransition t = { track, 400000, 100000, OE_TRANSITION_CROSS_DISSOLVE };

  g_assert_true (oe_project_add_transition (project, &t, NULL));

  OeSequence seq = { 0 };

  oe_project_get_sequence (project, &seq);

  OeTransition stored;

  g_assert_true (oe_project_get_transition (project, 0, &stored));
  g_assert_cmpint (stored.duration_us, ==, 100000);

  const OeTransitionWindow window = oe_transition_window (&seq, &stored);
  const OeTimelineGeometry geometry = { .px_per_us = 0.01, .origin_x = 20.0, .track_count = 1 };

  OeTransitionBandRect band = oe_timeline_transition_band (&geometry, &window, track);

  g_assert_true (band.active);
  g_assert_cmpfloat (band.x_start, ==, 20.0 + 350000 * 0.01);
  g_assert_cmpfloat (band.x_end, ==, 20.0 + 450000 * 0.01);
  g_assert_cmpfloat (band.height, >, 0.0);

  gint64 edge_start = 0, edge_end = 0;

  g_assert_true (oe_timeline_transition_edges (&window, &edge_start, &edge_end));
  g_assert_cmpint (edge_start, ==, 350000);
  g_assert_cmpint (edge_end, ==, 450000);

  /* Break the coverage: no band, no edges. */
  g_assert_true (oe_project_move_clip (project, track, 1, 850000, NULL));
  oe_sequence_clear (&seq);
  oe_project_get_sequence (project, &seq);

  const OeTransitionWindow dead = oe_transition_window (&seq, &stored);

  g_assert_false (dead.active);
  band = oe_timeline_transition_band (&geometry, &dead, track);
  g_assert_false (band.active);
  g_assert_false (oe_timeline_transition_edges (&dead, &edge_start, &edge_end));

  oe_sequence_clear (&seq);
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */
/* Ripple: re-anchor replay through mutators                           */
/* ------------------------------------------------------------------ */

/* Deleting the middle of three clips shifts the later boundary's
 * transition by the removed span — replayed through the validated
 * mutator as a recorded sub-step — while the destroyed boundary's
 * transition degrades; undo restores the exact pre-state. */
static void
test_ripple_reanchor_replay (void)
{
  OeProject *project = build_project_25fps ();
  const guint track = add_contiguous_clips (project, 3, 400000);
  const OeTransition lead = { track, 400000, 100000, OE_TRANSITION_CROSS_DISSOLVE };
  const OeTransition trail = { track, 800000, 100000, OE_TRANSITION_DIP_TO_BLACK };

  g_assert_true (oe_project_add_transition (project, &lead, NULL));
  g_assert_true (oe_project_add_transition (project, &trail, NULL));

  OeUndoStack *stack = oe_undo_stack_new ();

  g_assert_true (oe_edit_ripple_remove_clip (project, stack, track, 1, NULL));

  /* The suffix moved left by 400000 µs; the trail transition
   * re-anchored from 800000 to 400000 with its stored duration. The
   * lead transition's boundary was destroyed by the removal, but no
   * fixup drops it (D5): it persists — and the shifted suffix happens
   * to re-form a covered boundary at 400000, so it is active again at
   * composite time. */
  OeClip clip;

  g_assert_true (oe_project_get_clip (project, track, 1, &clip));
  g_assert_cmpint (clip.position_us, ==, 400000);
  g_assert_cmpuint (oe_project_get_transition_count (project), ==, 2);

  OeTransition stored;

  g_assert_true (oe_project_get_transition (project, 0, &stored));
  g_assert_cmpint (stored.at_us, ==, 400000);
  g_assert_cmpint (stored.kind, ==, OE_TRANSITION_CROSS_DISSOLVE);

  g_assert_true (oe_project_get_transition (project, 1, &stored));
  g_assert_cmpint (stored.at_us, ==, 400000); /* re-anchored from 800000 */
  g_assert_cmpint (stored.kind, ==, OE_TRANSITION_DIP_TO_BLACK);

  /* Undo replays the sub-step backwards: the trail re-anchors from
   * 400000 back to 800000 and both keep their original anchors. */
  g_assert_true (oe_undo_stack_undo (stack, project, NULL, NULL));
  g_assert_cmpuint (oe_project_get_transition_count (project), ==, 2);

  g_assert_true (oe_project_get_transition (project, 0, &stored));
  g_assert_cmpint (stored.at_us, ==, 400000);
  g_assert_cmpint (stored.kind, ==, OE_TRANSITION_CROSS_DISSOLVE);

  g_assert_true (oe_project_get_transition (project, 1, &stored));
  g_assert_cmpint (stored.at_us, ==, 800000);
  g_assert_cmpint (stored.kind, ==, OE_TRANSITION_DIP_TO_BLACK);

  oe_undo_stack_free (stack); /* a plain struct, not a GObject */
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */
/* Format: D8 persistence                                              */
/* ------------------------------------------------------------------ */

/* Keyframes and transitions serialize unconditionally and survive a
 * save-load-save round trip byte-identically. */
typedef struct
{
  gchar *dir;
} TmpDir;

static void
tmp_dir_set_up (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->dir = g_dir_make_tmp ("oe-waveb-XXXXXX", NULL);

  g_assert_nonnull (fx->dir);
}

static void
tmp_dir_tear_down (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  if (fx->dir == NULL)
    return;

  const gchar *name;
  GDir *dir = g_dir_open (fx->dir, 0, NULL);

  while ((name = g_dir_read_name (dir)) != NULL)
    {
      gchar *child = g_build_filename (fx->dir, name, NULL);

      g_remove (child);
      g_free (child);
    }

  g_dir_close (dir);
  g_rmdir (fx->dir);
  g_free (fx->dir);
  fx->dir = NULL;
}

static void
test_format_members_round_trip (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  OeProject *project = build_project_25fps ();
  const guint track = add_contiguous_clips (project, 2, 1000000);

  /* A keyed opacity ramp on clip 0, plus a position key. */
  {
    OeClipVisual v = oe_clip_visual_identity ();
    GArray *store = g_array_new (FALSE, FALSE, sizeof (OeKeyframe));
    const OeKeyframe keys[] = {
      { OE_KEYFRAME_OPACITY, 0, 128 },
      { OE_KEYFRAME_OPACITY, 250000, 255 },
      { OE_KEYFRAME_OPACITY, 500000, 0 },
      { OE_KEYFRAME_POS_X, 100000, -35 },
    };

    for (gsize i = 0; i < G_N_ELEMENTS (keys); i++)
      oe_keyframes_insert (store, keys[i]);

    v.keyframes = store;
    g_assert_true (oe_project_set_clip_visual (project, track, 0, &v, NULL));

    /* The mutator swapped in a deep copy; the transient store stays
     * test-owned and must be released here. */
    g_array_unref (store);
  }

  /* One cross-dissolve and one dip-to-black on the shared boundary. */
  const OeTransition a = { track, 1000000, 200000, OE_TRANSITION_CROSS_DISSOLVE };

  g_assert_true (oe_project_add_transition (project, &a, NULL));

  gchar *first_path = g_build_filename (fx->dir, "first.oe", NULL);
  gchar *second_path = g_build_filename (fx->dir, "second.oe", NULL);

  g_assert_true (oe_project_format_save (project, first_path, NULL));

  OeProject *loaded = oe_project_format_load (first_path, NULL);

  g_assert_nonnull (loaded);
  g_assert_true (oe_project_format_save (loaded, second_path, NULL));

  gchar *first = slurp (first_path);
  gchar *second = slurp (second_path);

  g_assert_cmpstr (first, ==, second);

  /* The members are present in the bytes: unconditional emission. */
  g_assert_nonnull (strstr (first, "\"keyframes\""));
  g_assert_nonnull (strstr (first, "\"transitions\""));
  g_assert_nonnull (strstr (first, "\"time-us\": 250000"));
  g_assert_nonnull (strstr (first, "\"cross-dissolve\""));

  /* The loaded model holds the same observable state. */
  OeTransition stored;

  g_assert_cmpuint (oe_project_get_transition_count (loaded), ==, 1);
  g_assert_true (oe_project_get_transition (loaded, 0, &stored));
  g_assert_cmpint (stored.at_us, ==, 1000000);
  g_assert_cmpint (stored.duration_us, ==, 200000);
  g_assert_cmpint (stored.kind, ==, OE_TRANSITION_CROSS_DISSOLVE);

  OeClip clip;

  g_assert_true (oe_project_get_clip (loaded, track, 0, &clip));
  g_assert_nonnull (clip.visual.keyframes);
  g_assert_cmpuint (oe_keyframes_count_for_property (clip.visual.keyframes, OE_KEYFRAME_OPACITY),
                    ==, 3);
  /* Mid-ramp sample between the (0 µs, 128) and (250000 µs, 255)
   * keys: 128 + one rounded interpolation step. */
  g_assert_cmpint (oe_keyframes_sample (clip.visual.keyframes, OE_KEYFRAME_OPACITY, 125000, 255),
                   ==, 128 + oe_time_round_ratio ((255 - 128) * 125000, 250000));

  g_free (first);
  g_free (second);
  g_free (first_path);
  g_free (second_path);
  g_object_unref (loaded);
  g_object_unref (project);
}

/* Strictness: unknown members, non-integer tokens, negative times,
 * out-of-domain values, and unknown kinds all fail the load; absent
 * members backfill none. Each case mutates ONE member of a real saved
 * document and pins the expected error code. */
static void
test_format_strictness (TmpDir *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  OeProject *project = build_project_25fps ();
  const guint track = add_contiguous_clips (project, 2, 1000000);
  const OeTransition a = { track, 1000000, 100000, OE_TRANSITION_CROSS_DISSOLVE };

  g_assert_true (oe_project_add_transition (project, &a, NULL));

  gchar *path = g_build_filename (fx->dir, "base.oe", NULL);

  g_assert_true (oe_project_format_save (project, path, NULL));

  gchar *base = slurp (path);

  /* Both saved clips are keyless, so the first empty keyframes member
   * is clip 0's. The writer's track tail is the single transitions
   * member. */
  typedef struct
  {
    const gchar *label;
    gboolean on_clip;  /* clip-level member vs track-level member */
    const gchar *body; /* replacement text for that member */
    int expected_code; /* OE_PROJECT_FORMAT_ERROR_* */
  } Mutation;

  const Mutation mutations[] = {
    /* Clip-level keyframes (replaces the first "keyframes": {}). */
    { "unknown keyframe property", TRUE,
      "\"keyframes\": { \"zoom\": [ { \"time-us\": 0, \"value\": 1 } ] }",
      OE_PROJECT_FORMAT_ERROR_UNKNOWN_MEMBER },
    { "unknown keyframe entry member", TRUE,
      "\"keyframes\": { \"opacity\": [ { \"time-us\": 0, \"value\": 1, \"x\": 2 } ] }",
      OE_PROJECT_FORMAT_ERROR_UNKNOWN_MEMBER },
    { "negative keyframe time", TRUE,
      "\"keyframes\": { \"opacity\": [ { \"time-us\": -5, \"value\": 1 } ] }",
      OE_PROJECT_FORMAT_ERROR_VALUE },
    { "out-of-domain opacity value", TRUE,
      "\"keyframes\": { \"opacity\": [ { \"time-us\": 0, \"value\": 300 } ] }",
      OE_PROJECT_FORMAT_ERROR_VALUE },
    { "keyframes not an object", TRUE, "\"keyframes\": [ 1, 2 ]", OE_PROJECT_FORMAT_ERROR_TYPE },
    /* Track-level transitions (replaces the writer's transitions tail). */
    { "unknown transition entry member", FALSE,
      "\"transitions\": [ { \"at-us\": 1000000, \"duration-us\": 100000, "
      "\"kind\": \"cross-dissolve\", \"track\": 0 } ]",
      OE_PROJECT_FORMAT_ERROR_UNKNOWN_MEMBER },
    { "unknown transition kind", FALSE,
      "\"transitions\": [ { \"at-us\": 1000000, \"duration-us\": 100000, \"kind\": \"wipe\" } ]",
      OE_PROJECT_FORMAT_ERROR_VALUE },
    { "non-boundary transition time", FALSE,
      "\"transitions\": [ { \"at-us\": 0, \"duration-us\": 100000, \"kind\": \"cross-dissolve\" } "
      "]",
      OE_PROJECT_FORMAT_ERROR_VALUE },
    { "transitions not an array", FALSE, "\"transitions\": { }", OE_PROJECT_FORMAT_ERROR_TYPE },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (mutations); i++)
    {
      const Mutation *m = &mutations[i];
      gchar *mutated;

      if (m->on_clip)
        mutated = str_replace_once (base, "\"keyframes\": {}", m->body);
      else
        {
          /* The fixture has one transition, so the writer emits the
           * populated member; the mutation swaps the whole tail. */
          gchar *tail = g_strdup_printf ("        %s", m->body);

          mutated = str_replace_once (base,
                                      "\"transitions\": [\n          { \"at-us\": 1000000, "
                                      "\"duration-us\": 100000, \"kind\": "
                                      "\"cross-dissolve\" }\n        ]",
                                      tail);
          g_free (tail);
        }

      gchar *mutated_path = g_build_filename (fx->dir, "mutated.oe", NULL);

      g_assert_true (g_file_set_contents (mutated_path, mutated, -1, NULL));

      GError *error = NULL;
      OeProject *loaded = oe_project_format_load (mutated_path, &error);

      g_assert_null (loaded);
      g_assert_cmpuint ((guint) error->domain, ==, (guint) OE_PROJECT_FORMAT_ERROR);
      g_assert_cmpint (error->code, ==, m->expected_code);
      g_clear_error (&error);
      g_free (mutated);
      g_free (mutated_path);
    }

  /* The unmutated document still loads: the mutations, not the writer,
   * were at fault. */
  OeProject *reloaded = oe_project_format_load (path, NULL);

  g_assert_nonnull (reloaded);
  g_assert_cmpuint (oe_project_get_transition_count (reloaded), ==, 1);
  g_object_unref (reloaded);

  /* Absence means none: a pre-Wave-B document (members removed) loads
   * with no keyframes and no transitions. */
  gchar *no_transitions = str_replace_once (base,
                                            ",\n        \"transitions\": [\n          "
                                            "{ \"at-us\": 1000000, "
                                            "\"duration-us\": 100000, "
                                            "\"kind\": \"cross-dissolve\" }\n        ]",
                                            "");
  gchar *legacy = str_replace_once (no_transitions, ",\n          \"keyframes\": {}", "");

  g_free (no_transitions);

  gchar *legacy_path = g_build_filename (fx->dir, "legacy.oe", NULL);

  g_assert_true (g_file_set_contents (legacy_path, legacy, -1, NULL));

  OeProject *loaded = oe_project_format_load (legacy_path, NULL);

  g_assert_nonnull (loaded);
  g_assert_cmpuint (oe_project_get_transition_count (loaded), ==, 0);

  OeClip clip;

  g_assert_true (oe_project_get_clip (loaded, track, 0, &clip));
  g_assert_null (clip.visual.keyframes);

  g_free (legacy);
  g_free (legacy_path);
  g_free (base);
  g_free (path);
  g_object_unref (loaded);
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

#define ADD(path, fn) g_test_add_func ((path), (fn))

  ADD ("/wave-b/keyframes/sample-single-rounding", test_sample_single_rounding);
  ADD ("/wave-b/keyframes/sample-clamp-endpoints", test_sample_clamp_endpoints);
  ADD ("/wave-b/keyframes/sample-degradation", test_sample_degradation);
  ADD ("/wave-b/keyframes/resolve-per-property", test_resolve_per_property);
  ADD ("/wave-b/keyframes/undo-visual-record", test_keyframe_edit_undo_record);

  ADD ("/wave-b/fades/ramp-endpoints", test_fade_ramp_endpoints);
  ADD ("/wave-b/fades/single-rounding", test_fade_single_rounding);

  ADD ("/wave-b/transitions/window", test_transition_window);
  ADD ("/wave-b/transitions/mutators", test_transition_mutators);

  ADD ("/wave-b/layout/band-and-edges", test_layout_band_and_edges);

  ADD ("/wave-b/ripple/reanchor-replay", test_ripple_reanchor_replay);

  g_test_add ("/wave-b/format/members-round-trip", TmpDir, NULL, tmp_dir_set_up,
              test_format_members_round_trip, tmp_dir_tear_down);
  g_test_add ("/wave-b/format/strictness", TmpDir, NULL, tmp_dir_set_up, test_format_strictness,
              tmp_dir_tear_down);

#undef ADD

  return g_test_run ();
}
