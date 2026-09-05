/* test_timeline_layout.c — pure geometry: conversions, lanes, hit
 * testing, and drag clamps. Fixtures build real OeProject models and
 * pass oe_project_get_sequence() copies, exactly what the widget will
 * hand over from its observer callback.
 *
 * Cases:
 *   /timeline-layout/conversions   x/us round trips; left-of-origin
 *                                  clamps to 0; zoom is honoured.
 *   /timeline-layout/lanes         ruler + track lane y mapping, both
 *                                  directions.
 *   /timeline-layout/hit-test      body → MOVE, edge band → TRIM,
 *                                  ruler, gaps, and past-the-end.
 *   /timeline-layout/clamp-move    wanted / neighbour snaps / floor,
 *                                  adjacency legal, ties prefer left.
 *   /timeline-layout/trim-bounds   AV bounded by the probed duration,
 *                                  stills unbounded, 1 µs floor.
 *   /timeline-layout/clip-label    kind-aware labels (Wave B): title
 *                                  text, solid hex, media basename;
 *                                  plus the hex parse/format pair.
 */

#include <glib.h>

#include <string.h>

#include "../src/ui/oe_timeline_layout.h"

/* One video track with three clips (footprints in µs):
 * A [1 s, 2 s), B [3 s, 4 s), C [4 s, 4.5 s) — C adjacent to B. */
static OeProject *
build_hit_fixture (void)
{
  OeProject *project = oe_project_new_default ();

  oe_project_add_media (project, "/media/a.mp4");
  oe_project_add_track (project, OE_TRACK_VIDEO);

  g_assert_true (oe_project_insert_clip (project, 0, 1, 1000000, 0, 1000000, NULL));
  g_assert_true (oe_project_insert_clip (project, 0, 1, 3000000, 0, 1000000, NULL));
  g_assert_true (oe_project_insert_clip (project, 0, 1, 4000000, 0, 500000, NULL));

  return project;
}

/* 100 px per second (0.0001 px/µs), origin at x = 40: 1 s = 100 px. */
static OeTimelineGeometry
geometry_100px (guint track_count)
{
  OeTimelineGeometry geometry = { 0.0001, 40.0, track_count };

  return geometry;
}

static void
test_conversions (void)
{
  OeTimelineGeometry geometry = geometry_100px (1);

  g_assert_cmpfloat (oe_timeline_x_for_us (&geometry, 0), ==, 40.0);
  g_assert_cmpfloat (oe_timeline_x_for_us (&geometry, 1000000), ==, 140.0);

  g_assert_cmpint (oe_timeline_us_for_x (&geometry, 140.0), ==, 1000000);
  g_assert_cmpint (oe_timeline_us_for_x (&geometry, 40.0), ==, 0);
  g_assert_cmpint (oe_timeline_us_for_x (&geometry, 39.9), ==, 0); /* clamped */

  /* Round trip across a spread of positions. */
  const gint64 positions[] = { 0, 1, 999, 1000000, 2730000, 60000000 };

  for (guint i = 0; i < G_N_ELEMENTS (positions); i++)
    {
      gdouble x = oe_timeline_x_for_us (&geometry, positions[i]);

      g_assert_cmpint (oe_timeline_us_for_x (&geometry, x), ==, positions[i]);
    }

  /* Sub-pixel pointer wobble rounds to the nearest microsecond at a
   * coarse zoom (1 px = 1 µs) where the difference is observable. */
  OeTimelineGeometry coarse = { 1.0, 0.0, 1 };

  g_assert_cmpint (oe_timeline_us_for_x (&coarse, 10.4), ==, 10);
  g_assert_cmpint (oe_timeline_us_for_x (&coarse, 10.6), ==, 11);
}

static void
test_lanes (void)
{
  OeTimelineGeometry geometry = geometry_100px (2);

  g_assert_cmpfloat (oe_timeline_y_for_track (&geometry, 0), ==, OE_TIMELINE_RULER_HEIGHT);
  g_assert_cmpfloat (oe_timeline_y_for_track (&geometry, 1), ==,
                     OE_TIMELINE_RULER_HEIGHT + OE_TIMELINE_TRACK_HEIGHT);

  g_assert_cmpint (oe_timeline_track_index_for_y (&geometry, 10.0), ==, -1); /* ruler */
  g_assert_cmpint (oe_timeline_track_index_for_y (&geometry, (gdouble) OE_TIMELINE_RULER_HEIGHT),
                   ==, 0);
  g_assert_cmpint (oe_timeline_track_index_for_y (&geometry, 55.0), ==, 0);
  g_assert_cmpint (oe_timeline_track_index_for_y (&geometry, 80.0), ==, 1);
  g_assert_cmpint (oe_timeline_track_index_for_y (&geometry, 200.0), ==, -1); /* past lanes */
}

static void
test_hit_test (void)
{
  OeProject *project = build_hit_fixture ();
  OeSequence sequence = { 0 }; /* zeroed storage per get_sequence contract */

  oe_project_get_sequence (project, &sequence);

  OeTimelineGeometry geometry = geometry_100px (1);

  /* Clip A body: 1 s @ x 140 → MOVE on track 0, clip 0. */
  OeTimelineHit hit = oe_timeline_hit_test (&geometry, &sequence, 150.0, 40.0);

  g_assert_cmpint (hit.kind, ==, OE_TIMELINE_HIT_MOVE);
  g_assert_cmpuint (hit.track_index, ==, 0);
  g_assert_cmpuint (hit.clip_index, ==, 0);

  /* Edge bands: both edges of clip A (x 140, x 240). */
  hit = oe_timeline_hit_test (&geometry, &sequence, 141.0, 40.0);
  g_assert_cmpint (hit.kind, ==, OE_TIMELINE_HIT_TRIM_IN);

  hit = oe_timeline_hit_test (&geometry, &sequence, 239.0, 40.0);
  g_assert_cmpint (hit.kind, ==, OE_TIMELINE_HIT_TRIM_OUT);

  /* Clip B body: x [340, 440) → MOVE on clip 1. */
  hit = oe_timeline_hit_test (&geometry, &sequence, 400.0, 40.0);
  g_assert_cmpint (hit.kind, ==, OE_TIMELINE_HIT_MOVE);
  g_assert_cmpuint (hit.clip_index, ==, 1);

  /* Just outside the band: a move again. */
  hit = oe_timeline_hit_test (&geometry, &sequence, 147.0, 40.0);
  g_assert_cmpint (hit.kind, ==, OE_TIMELINE_HIT_MOVE);

  /* Ruler, gap, and dead space past the lane. */
  hit = oe_timeline_hit_test (&geometry, &sequence, 150.0, 10.0);
  g_assert_cmpint (hit.kind, ==, OE_TIMELINE_HIT_RULER);

  hit = oe_timeline_hit_test (&geometry, &sequence, 250.0, 40.0);
  g_assert_cmpint (hit.kind, ==, OE_TIMELINE_HIT_NOTHING);

  hit = oe_timeline_hit_test (&geometry, &sequence, 150.0, 200.0);
  g_assert_cmpint (hit.kind, ==, OE_TIMELINE_HIT_NOTHING);

  oe_sequence_clear (&sequence);
  g_clear_object (&project);
}

static void
test_clamp_move (void)
{
  OeProject *project = build_hit_fixture ();
  OeSequence sequence = { 0 }; /* zeroed storage per get_sequence contract */

  oe_project_get_sequence (project, &sequence);

  /* Move clip B [3 s, 4 s): wanted inside C (overlap) snaps to C's
   * right edge — nearest legal candidate. */
  g_assert_cmpint (oe_timeline_clamp_move_position (&sequence, 0, 1, 4200000), ==, 4500000);

  /* Wanted just left of B's home, overlapping nothing: stays. */
  g_assert_cmpint (oe_timeline_clamp_move_position (&sequence, 0, 1, 2800000), ==, 2800000);

  /* Wanted overlapping A from the left snaps to A's end (adjacent). */
  g_assert_cmpint (oe_timeline_clamp_move_position (&sequence, 0, 1, 1500000), ==, 2000000);

  /* Negative wanted floors at 0. */
  g_assert_cmpint (oe_timeline_clamp_move_position (&sequence, 0, 1, -5000), ==, 0);

  oe_sequence_clear (&sequence);
  g_clear_object (&project);
}

static void
test_trim_bounds (void)
{
  OeProject *project = build_hit_fixture ();
  OeSequence sequence = { 0 }; /* zeroed storage per get_sequence contract */

  oe_project_get_sequence (project, &sequence);

  OeTrack *track = g_ptr_array_index (sequence.tracks, 0);
  OeClip *clip = g_ptr_array_index (track->clips, 0); /* A [0, 1 s) source */

  gint64 min_in = 0, max_in = 0, min_out = 0, max_out = 0;

  /* AV media annotated with a 2 s probed source. */
  oe_timeline_trim_bounds (clip, 2000000, &min_in, &max_in, &min_out, &max_out);

  g_assert_cmpint (min_in, ==, 0);
  g_assert_cmpint (max_in, ==, clip->source_out_us - OE_TIMELINE_MIN_TRIM_US);
  g_assert_cmpint (min_out, ==, clip->source_in_us + OE_TIMELINE_MIN_TRIM_US);
  g_assert_cmpint (max_out, ==, 2000000);

  /* Still media (or unprobed): unbounded. */
  oe_timeline_trim_bounds (clip, 0, &min_in, &max_in, &min_out, &max_out);
  g_assert_cmpint (max_out, ==, G_MAXINT64);

  oe_sequence_clear (&sequence);
  g_clear_object (&project);
}

/* Wave B: the model-side clip label — the string the widget draws for
 * a clip. Built over plain stack clips: the label helper reads kind
 * and generator payload only. */
static void
assert_label_is (OeClipKind kind, const gchar *text, gint color_rgb, const gchar *media_path,
                 const gchar *expected)
{
  OeClip clip;

  memset (&clip, 0, sizeof (clip));
  clip.kind = kind;
  clip.generator.color_rgb = color_rgb;
  if (text != NULL)
    clip.generator.text = g_strdup (text);

  g_autofree gchar *label = oe_timeline_clip_label (&clip, media_path);

  if (expected == NULL)
    g_assert_null (label);
  else
    g_assert_cmpstr (label, ==, expected);

  oe_clip_generator_clear (&clip.generator);
}

static void
test_clip_label (void)
{
  /* Titles label from their payload text... */
  assert_label_is (OE_CLIP_TITLE, "Hello", 0, NULL, "Hello");

  /* ...and never lose their label to an empty or cleared entry. */
  assert_label_is (OE_CLIP_TITLE, "", 0, NULL, "Title");
  assert_label_is (OE_CLIP_TITLE, NULL, 0, NULL, "Title");

  /* Solids label as the packed color, zero-padded lowercase hex. */
  assert_label_is (OE_CLIP_SOLID, NULL, 0xff8800, NULL, "#ff8800");
  assert_label_is (OE_CLIP_SOLID, NULL, 0x00ff00, NULL, "#00ff00");
  assert_label_is (OE_CLIP_SOLID, NULL, 0x000abc, NULL, "#000abc");
  assert_label_is (OE_CLIP_SOLID, NULL, 0, NULL, "#000000");

  /* Media labels the basename; no path draws no label. */
  assert_label_is (OE_CLIP_MEDIA, NULL, 0, "/media/a/clip.mp4", "clip.mp4");
  assert_label_is (OE_CLIP_MEDIA, NULL, 0, NULL, NULL);
  assert_label_is (OE_CLIP_MEDIA, NULL, 0, "", NULL);
}

static void
test_clip_color_hex (void)
{
  /* Format: lowercase, zero-padded, masked to 24 bits. */
  g_autofree gchar *hex = oe_timeline_clip_color_hex (0xff8800);

  g_assert_cmpstr (hex, ==, "#ff8800");
  g_free (g_steal_pointer (&hex));

  hex = oe_timeline_clip_color_hex (0x1ff8800); /* above the packed domain */
  g_assert_cmpstr (hex, ==, "#ff8800");
  g_free (g_steal_pointer (&hex));

  /* Parse: optional '#', exactly six hex digits, any case. */
  gint color = -1;

  g_assert_true (oe_timeline_clip_color_parse_hex ("ff8800", &color));
  g_assert_cmpint (color, ==, 0xff8800);
  g_assert_true (oe_timeline_clip_color_parse_hex ("#FF8800", &color));
  g_assert_cmpint (color, ==, 0xff8800);
  g_assert_true (oe_timeline_clip_color_parse_hex ("00ff00", &color));
  g_assert_cmpint (color, ==, 0x00ff00);

  /* Out may be NULL when the caller only validates. */
  g_assert_true (oe_timeline_clip_color_parse_hex ("abcdef", NULL));

  /* Rejects: too short, too long, non-hex, empty, NULL. */
  g_assert_false (oe_timeline_clip_color_parse_hex ("ff88", &color));
  g_assert_false (oe_timeline_clip_color_parse_hex ("ff88000", &color));
  g_assert_false (oe_timeline_clip_color_parse_hex ("ff88g0", &color));
  g_assert_false (oe_timeline_clip_color_parse_hex ("ff88g", &color));
  g_assert_false (oe_timeline_clip_color_parse_hex ("", &color));
  g_assert_false (oe_timeline_clip_color_parse_hex ("#", &color));
  g_assert_false (oe_timeline_clip_color_parse_hex (NULL, &color));

  /* A rejection must not touch the out parameter. */
  color = 42;
  g_assert_false (oe_timeline_clip_color_parse_hex ("zzzzzz", &color));
  g_assert_cmpint (color, ==, 42);

  /* Round trip. */
  g_autofree gchar *rt = oe_timeline_clip_color_hex (0x123456);

  g_assert_true (oe_timeline_clip_color_parse_hex (rt, &color));
  g_assert_cmpint (color, ==, 0x123456);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/timeline-layout/conversions", test_conversions);
  g_test_add_func ("/timeline-layout/lanes", test_lanes);
  g_test_add_func ("/timeline-layout/hit-test", test_hit_test);
  g_test_add_func ("/timeline-layout/clamp-move", test_clamp_move);
  g_test_add_func ("/timeline-layout/trim-bounds", test_trim_bounds);
  g_test_add_func ("/timeline-layout/clip-label", test_clip_label);
  g_test_add_func ("/timeline-layout/clip-color-hex", test_clip_color_hex);

  return g_test_run ();
}
