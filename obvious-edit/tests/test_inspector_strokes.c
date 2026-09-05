/* test_inspector_strokes.c — GTK-free tests for Phase 11 Wave B
 * (20th suite): the inspector's stroke contract, replayed through the
 * same validated mutators and undo helpers the GTK window calls.
 *
 * The clip page's Wave B sections (generated-clip text/size/color and
 * the media-clip chroma key) ride one contract: a baseline is captured
 * at the stroke's first change, live previews mutate the model WITHOUT
 * a record, and the stroke's commit — activate, focus-out, grid-gesture
 * release, or reselect — records exactly ONE undo entry restoring the
 * baseline; a zero-delta stroke records nothing; a rejected commit
 * leaves the model's truth in place and records nothing. The window
 * implementation is thin GTK glue over these calls, so the semantics
 * are tested at this seam.
 *
 *   /inspector-strokes/key-record-per-stroke   preview without record;
 *                                               ONE CLIP_KEY at commit;
 *                                               undo restores baseline
 *   /inspector-strokes/key-zero-delta-silent   a stroke that ends where
 *                                               it began records nothing
 *   /inspector-strokes/key-rejected-silent     keying a generator clip
 *                                               is rejected: model truth
 *                                               kept, no record
 *   /inspector-strokes/generator-record-per-stroke  ONE GENERATOR at
 *                                               commit (owned text)
 *   /inspector-strokes/generator-zero-delta-silent  same-state commit
 *                                               records nothing
 *   /inspector-strokes/generator-rejected-silent  an invalid payload at
 *                                               commit leaves the model
 *                                               truth and records nothing
 *
 * Links the project and undo-stack sources only — no GTK, no Cairo.
 */

#include <glib.h>
#include <string.h>

#include "../src/app/oe_undo_stack.h"
#include "../src/core/oe_project.h"

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

/* 25 fps 320x240 sequence — the geometry the window tests use. */
static OeProject *
new_project_25fps (void)
{
  OeProject *project = oe_project_new ((OeRational) { 25, 1 });

  g_assert_cmpint (oe_project_set_sequence_size (project, 320, 240, NULL), !=, 0);
  return project;
}

/* Inserts a generated clip through the validated mutator, the same
 * path the insert commands use. Asserts success — test-infra failure,
 * not a test subject. */
static void
insert_generator (OeProject *project, guint track, OeClipKind kind, const OeClipGenerator *gen,
                  gint64 position_us, gint64 duration_us)
{
  GError *error = NULL;

  g_assert_true (oe_project_insert_generator_clip (project, track, kind, position_us, duration_us,
                                                   gen, &error));
  g_assert_no_error (error);
}

/* Inserts a keyed-able media clip (ref 1, a still-style unbounded
 * source) on a video track. */
static void
insert_media (OeProject *project, guint track, gint64 position_us, gint64 duration_us)
{
  GError *error = NULL;

  g_assert_true (oe_project_add_media (project, "/nonexistent/still.png") == 1);
  g_assert_true (oe_project_insert_clip (project, track, 1, position_us, 0, duration_us, &error));
  g_assert_no_error (error);
}

/* ------------------------------------------------------------------ */
/* Stroke drivers — the window's clip-generator/clip-key functions,    */
/* GTK-free: begin captures the baseline, preview mutates without a    */
/* record, commit lands exactly one entry through the _with_old        */
/* helper.                                                             */
/* ------------------------------------------------------------------ */

static void
stroke_key_begin (OeProject *project, guint track, guint clip, OeClipKey *baseline)
{
  OeClip c;

  g_assert_true (oe_project_get_clip (project, track, clip, &c));
  *baseline = c.key;
}

static void
stroke_key_preview (OeProject *project, guint track, guint clip, const OeClipKey *live)
{
  GError *error = NULL;

  g_assert_true (oe_project_set_clip_key (project, track, clip, live, &error));
  g_assert_no_error (error);
}

static void
stroke_key_commit (OeProject *project, OeUndoStack *stack, guint track, guint clip,
                   const OeClipKey *baseline, const OeClipKey *live)
{
  GError *error = NULL;

  g_assert_true (
      oe_edit_set_clip_key_with_old (project, stack, track, clip, baseline, live, &error));
  g_assert_no_error (error);
}

static void
stroke_generator_begin (OeProject *project, guint track, guint clip, OeClipGenerator *baseline)
{
  OeClip c;

  *baseline = oe_clip_generator_identity (); /* copy frees dst's text first */
  g_assert_true (oe_project_get_clip (project, track, clip, &c));
  oe_clip_generator_copy (baseline, &c.generator); /* get_clip borrows members */
}

static void
stroke_generator_preview (OeProject *project, guint track, guint clip, const OeClipGenerator *live)
{
  GError *error = NULL;

  g_assert_true (oe_project_set_clip_generator (project, track, clip, live, &error));
  g_assert_no_error (error);
}

static void
stroke_generator_commit (OeProject *project, OeUndoStack *stack, guint track, guint clip,
                         const OeClipGenerator *baseline, const OeClipGenerator *live)
{
  GError *error = NULL;

  g_assert_true (
      oe_edit_set_clip_generator_with_old (project, stack, track, clip, baseline, live, &error));
  g_assert_no_error (error);
}

/* ------------------------------------------------------------------ */
/* Key strokes                                                         */
/* ------------------------------------------------------------------ */

static void
test_key_record_per_stroke (void)
{
  OeProject *project = new_project_25fps ();
  OeUndoStack *stack = oe_undo_stack_new ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);

  insert_media (project, video, 0, 1000000);

  /* A stroke that previews twice (the user dragging tolerance) and
   * commits once records exactly ONE entry. */
  OeClipKey baseline;

  stroke_key_begin (project, video, 0, &baseline);

  OeClipKey live = baseline;

  live.tolerance = 100;
  stroke_key_preview (project, video, 0, &live);
  live.tolerance = 220;
  live.softness = 40;
  stroke_key_preview (project, video, 0, &live);

  /* Preview mutated the model without a record. */
  OeClip c;

  g_assert_true (oe_project_get_clip (project, video, 0, &c));
  g_assert_cmpint (c.key.tolerance, ==, 220);
  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 0);

  stroke_key_commit (project, stack, video, 0, &baseline, &live);
  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 1);

  /* Undo restores the stroke-begin state through the model. */
  g_assert_true (oe_undo_stack_undo (stack, project, NULL, NULL));
  g_assert_true (oe_project_get_clip (project, video, 0, &c));
  g_assert_cmpint (c.key.tolerance, ==, baseline.tolerance);
  g_assert_cmpint (c.key.softness, ==, baseline.softness);
  g_assert_cmpint (c.key.color_rgb, ==, baseline.color_rgb);
  g_assert_cmpint (c.key.enabled, ==, baseline.enabled);

  oe_undo_stack_free (stack);
  g_object_unref (project);
}

static void
test_key_zero_delta_silent (void)
{
  OeProject *project = new_project_25fps ();
  OeUndoStack *stack = oe_undo_stack_new ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);

  insert_media (project, video, 0, 1000000);

  /* The user typed the same values the model already held: the
   * commit records nothing. */
  OeClipKey baseline;

  stroke_key_begin (project, video, 0, &baseline);
  stroke_key_commit (project, stack, video, 0, &baseline, &baseline);

  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 0);
  g_assert_false (oe_undo_stack_can_undo (stack));

  oe_undo_stack_free (stack);
  g_object_unref (project);
}

static void
test_key_rejected_silent (void)
{
  OeProject *project = new_project_25fps ();
  OeUndoStack *stack = oe_undo_stack_new ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);

  OeClipGenerator gen = oe_clip_generator_identity ();

  gen.text = g_strdup ("Title");
  gen.color_rgb = 0xffffff;
  gen.size_permille = 150;
  insert_generator (project, video, OE_CLIP_TITLE, &gen, 0, 1000000);
  oe_clip_generator_clear (&gen);

  /* Keying a generator clip is rejected (spec D4): the model keeps
   * its truth, nothing is recorded — the window reloads the section
   * from the model and carries on. */
  OeClipKey baseline, live;

  stroke_key_begin (project, video, 0, &baseline);
  live = baseline;
  live.tolerance = 512;

  GError *error = NULL;

  g_assert_false (
      oe_edit_set_clip_key_with_old (project, stack, video, 0, &baseline, &live, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_GENERATOR);
  g_clear_error (&error);

  OeClip c;

  g_assert_true (oe_project_get_clip (project, video, 0, &c));
  g_assert_cmpint (c.kind, ==, OE_CLIP_TITLE);
  g_assert_cmpstr (c.generator.text, ==, "Title");
  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 0);

  oe_undo_stack_free (stack);
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */
/* Generator strokes                                                   */
/* ------------------------------------------------------------------ */

static void
test_generator_record_per_stroke (void)
{
  OeProject *project = new_project_25fps ();
  OeUndoStack *stack = oe_undo_stack_new ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);

  OeClipGenerator gen = oe_clip_generator_identity ();

  gen.text = g_strdup ("Before");
  gen.color_rgb = 0xffffff;
  gen.size_permille = 150;
  insert_generator (project, video, OE_CLIP_TITLE, &gen, 0, 1000000);
  oe_clip_generator_clear (&gen);

  /* Typing edits the text per keystroke: several previews, one
   * commit, one record restoring the pre-stroke text bit-exactly. */
  OeClipGenerator baseline;

  stroke_generator_begin (project, video, 0, &baseline);

  OeClipGenerator live = baseline;

  live.text = g_strdup ("Bef");
  stroke_generator_preview (project, video, 0, &live);
  oe_clip_generator_clear (&live);

  live = baseline;
  live.text = g_strdup ("Hello");
  live.color_rgb = 0x00ff00;
  live.size_permille = 300;
  stroke_generator_preview (project, video, 0, &live);

  OeClip c;

  g_assert_true (oe_project_get_clip (project, video, 0, &c));
  g_assert_cmpstr (c.generator.text, ==, "Hello");
  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 0);

  stroke_generator_commit (project, stack, video, 0, &baseline, &live);
  oe_clip_generator_clear (&live);
  oe_clip_generator_clear (&baseline);
  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 1);

  g_assert_true (oe_undo_stack_undo (stack, project, NULL, NULL));
  g_assert_true (oe_project_get_clip (project, video, 0, &c));
  g_assert_cmpstr (c.generator.text, ==, "Before");
  g_assert_cmpint (c.generator.color_rgb, ==, 0xffffff);
  g_assert_cmpint (c.generator.size_permille, ==, 150);

  oe_undo_stack_free (stack);
  g_object_unref (project);
}

static void
test_generator_zero_delta_silent (void)
{
  OeProject *project = new_project_25fps ();
  OeUndoStack *stack = oe_undo_stack_new ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);

  OeClipGenerator gen = oe_clip_generator_identity ();

  gen.text = g_strdup ("Same");
  gen.color_rgb = 0xffffff;
  gen.size_permille = 150;
  insert_generator (project, video, OE_CLIP_TITLE, &gen, 0, 1000000);
  oe_clip_generator_clear (&gen);

  /* Focus-out with no edits: the commit sees equal states and
   * records nothing. */
  OeClipGenerator baseline;

  stroke_generator_begin (project, video, 0, &baseline);

  OeClipGenerator live;

  live.text = g_strdup ("Same");
  live.color_rgb = baseline.color_rgb;
  live.size_permille = baseline.size_permille;
  stroke_generator_commit (project, stack, video, 0, &baseline, &live);
  oe_clip_generator_clear (&live);
  oe_clip_generator_clear (&baseline);

  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 0);
  g_assert_false (oe_undo_stack_can_undo (stack));

  oe_undo_stack_free (stack);
  g_object_unref (project);
}

static void
test_generator_rejected_silent (void)
{
  OeProject *project = new_project_25fps ();
  OeUndoStack *stack = oe_undo_stack_new ();
  const guint video = oe_project_add_track (project, OE_TRACK_VIDEO);

  OeClipGenerator gen = oe_clip_generator_identity ();

  gen.text = g_strdup ("Title");
  gen.size_permille = 150;
  insert_generator (project, video, OE_CLIP_TITLE, &gen, 0, 1000000);
  oe_clip_generator_clear (&gen);

  /* A commit whose collected payload is invalid (size out of range)
   * is rejected by the validated mutator: the model keeps its truth
   * and nothing is recorded. */
  OeClipGenerator baseline;

  stroke_generator_begin (project, video, 0, &baseline);

  OeClipGenerator live = baseline;

  live.text = g_strdup ("Broken");
  live.size_permille = 0; /* invalid: the mutator must reject */

  GError *error = NULL;

  g_assert_false (
      oe_edit_set_clip_generator_with_old (project, stack, video, 0, &baseline, &live, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_GENERATOR);
  g_clear_error (&error);

  OeClip c;

  g_assert_true (oe_project_get_clip (project, video, 0, &c));
  g_assert_cmpstr (c.generator.text, ==, "Title");
  g_assert_cmpint (c.generator.size_permille, ==, 150);
  g_assert_cmpuint (oe_undo_stack_get_size (stack), ==, 0);

  oe_clip_generator_clear (&live);
  oe_clip_generator_clear (&baseline);
  oe_undo_stack_free (stack);
  g_object_unref (project);
}

/* ------------------------------------------------------------------ */

int
main (int argc, char *argv[])
{
#if !GLIB_CHECK_VERSION(2, 35, 0)
  g_type_init ();
#endif
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/inspector-strokes/key-record-per-stroke", test_key_record_per_stroke);
  g_test_add_func ("/inspector-strokes/key-zero-delta-silent", test_key_zero_delta_silent);
  g_test_add_func ("/inspector-strokes/key-rejected-silent", test_key_rejected_silent);
  g_test_add_func ("/inspector-strokes/generator-record-per-stroke",
                   test_generator_record_per_stroke);
  g_test_add_func ("/inspector-strokes/generator-zero-delta-silent",
                   test_generator_zero_delta_silent);
  g_test_add_func ("/inspector-strokes/generator-rejected-silent", test_generator_rejected_silent);

  return g_test_run ();
}
