/* test_project.c — GTK-free tests for the project & timeline model (Phase 3).
 *
 *   /project/defaults              new_default: Untitled, 25/1, empty.
 *   /project/track-ordering        video array order IS compositing order.
 *   /project/insert-sorted         clips stay ordered by position.
 *   /project/overlap-rejection     overlaps fail with a typed error;
 *                                  adjacency is allowed.
 *   /project/clip-duration-rule    timeline duration is out-in for
 *                                  every clip (stills included).
 *   /project/move-and-remove       move keeps order and rejects new
 *                                  overlaps; removal leaves a gap.
 *   /project/observer-once         every mutation fires the observer
 *                                  exactly once, and never from dispose.
 *   /project/deep-copy-getters     get_sequence hands out a copy the
 *                                  model cannot see or be hurt by.
 *   /project/media-refs            file-stable refs: sequential, explicit,
 *                                  duplicate and unknown rejections.
 *   /project/trim                 both edges trim in place; position
 *                                 untouched; AV bounded by the probed
 *                                 source duration.
 *   /project/trim-rejections      empty, negative, or beyond-media
 *                                 ranges and bad indices fail with
 *                                 typed errors; observer stays silent.
 *   /project/trim-still-extension stills extend freely (uniform-
 *                                 duration rule) but never across a
 *                                 neighbour.
 *   /project/trim-observer        a successful trim notifies exactly
 *                                 once; failures never notify.
 *   /project/destruction-order     observer cleared before teardown.
 */

#include <glib.h>

#include "../src/core/oe_project.h"

/* Observer harness: counts notifications. */
typedef struct
{
  guint count;
  gboolean notified_from_dispose;
} Observer;

static void
observer_count (gpointer user_data)
{
  Observer *observer = user_data;

  observer->count++;
}

static Observer *
observer_new (void)
{
  return g_new0 (Observer, 1);
}

/* Fetches the clip at (track, clip) from a fresh deep copy. */
static OeClip
get_clip (OeProject *project, guint track, guint clip)
{
  OeSequence sequence;

  oe_project_get_sequence (project, &sequence);
  OeClip result = *(OeClip *) g_ptr_array_index (
      ((OeTrack *) g_ptr_array_index (sequence.tracks, track))->clips, clip);
  oe_sequence_clear (&sequence);
  return result;
}

/* Builds a two-track project with three media refs. */
static OeProject *
build_populated (void)
{
  OeProject *project = oe_project_new_default ();

  oe_project_set_name (project, "Test");
  oe_project_add_media (project, "/media/a.mp4");
  oe_project_add_media (project, "/media/b.wav");
  oe_project_add_track (project, OE_TRACK_VIDEO);
  oe_project_add_track (project, OE_TRACK_AUDIO);

  return project;
}

/* --- defaults ----------------------------------------------------------- */

static void
test_defaults (void)
{
  OeProject *project = oe_project_new_default ();

  g_assert_cmpstr (oe_project_get_name (project), ==, "Untitled");
  g_assert_cmpuint (oe_project_get_track_count (project), ==, 0);
  g_assert_cmpuint (oe_project_get_media_count (project), ==, 0);

  OeSequence sequence;

  oe_project_get_sequence (project, &sequence);

  g_assert_cmpint (sequence.frame_rate.num, ==, OE_PROJECT_DEFAULT_RATE_NUM);
  g_assert_cmpint (sequence.frame_rate.den, ==, OE_PROJECT_DEFAULT_RATE_DEN);
  g_assert_cmpuint (sequence.tracks->len, ==, 0);
  oe_sequence_clear (&sequence);

  g_clear_object (&project);
}

/* --- track ordering ------------------------------------------------------ */

static void
test_track_ordering (void)
{
  OeProject *project = build_populated ();

  g_assert_cmpuint (oe_project_get_track_count (project), ==, 2);

  OeSequence sequence;

  oe_project_get_sequence (project, &sequence);

  g_assert_cmpuint (sequence.tracks->len, ==, 2);
  g_assert_cmpint (((OeTrack *) g_ptr_array_index (sequence.tracks, 0))->kind, ==, OE_TRACK_VIDEO);
  g_assert_cmpint (((OeTrack *) g_ptr_array_index (sequence.tracks, 1))->kind, ==, OE_TRACK_AUDIO);
  oe_sequence_clear (&sequence);

  g_clear_object (&project);
}

/* --- insertion keeps position order -------------------------------------- */

static void
test_insert_sorted (void)
{
  OeProject *project = build_populated ();

  /* Insert out of order; the track keeps them sorted. */
  g_assert_true (oe_project_insert_clip (project, 0, 1, 2000000, 0, 1000000, NULL));
  g_assert_true (oe_project_insert_clip (project, 0, 1, 0, 0, 1000000, NULL));
  g_assert_true (oe_project_insert_clip (project, 0, 1, 3000000, 0, 1000000, NULL));

  g_assert_cmpint (get_clip (project, 0, 0).position_us, ==, 0);
  g_assert_cmpint (get_clip (project, 0, 1).position_us, ==, 2000000);
  g_assert_cmpint (get_clip (project, 0, 2).position_us, ==, 3000000);

  g_clear_object (&project);
}

/* --- overlap rejection ---------------------------------------------------- */

static void
test_overlap_rejection (void)
{
  OeProject *project = build_populated ();

  /* Anchor clip at [500000, 1500000). */
  g_assert_true (oe_project_insert_clip (project, 0, 1, 500000, 0, 1000000, NULL));

  GError *error = NULL;

  /* Overlaps are rejected in every flavor: tail, head, contained,
   * containing — the error names the clip it hit. */
  g_assert_false (oe_project_insert_clip (project, 0, 1, 1000000, 0, 1000000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP);
  g_clear_error (&error);

  g_assert_false (oe_project_insert_clip (project, 0, 1, 0, 0, 1000000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP);
  g_clear_error (&error);

  g_assert_false (oe_project_insert_clip (project, 0, 1, 600000, 0, 50000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP);
  g_clear_error (&error);

  g_assert_false (oe_project_insert_clip (project, 0, 1, 0, 0, 2000000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP);
  g_clear_error (&error);

  /* A negative placement is a range defect, not an overlap. */
  g_assert_false (oe_project_insert_clip (project, 0, 1, -500000, 0, 500000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE);
  g_clear_error (&error);

  /* Adjacency (end == next start) is fine — on either side. */
  g_assert_true (oe_project_insert_clip (project, 0, 1, 1500000, 0, 500000, NULL));
  g_assert_true (oe_project_insert_clip (project, 0, 1, 0, 0, 500000, NULL));

  /* Same range on a different track never collides. */
  g_assert_true (oe_project_insert_clip (project, 1, 2, 500000, 0, 1000000, NULL));

  g_clear_object (&project);
}

/* --- the uniform duration rule (stills included) -------------------------- */

static void
test_clip_duration_rule (void)
{
  OeProject *project = build_populated ();

  /* A still image's "source range" encodes its screen duration: the
   * model applies out-in uniformly, no special-casing by kind. */
  g_assert_true (
      oe_project_insert_clip (project, 0, 1, 0, 0, OE_PROJECT_STILL_DEFAULT_DURATION_US, NULL));

  OeClip clip = get_clip (project, 0, 0);

  g_assert_cmpint (clip.source_out_us - clip.source_in_us, ==,
                   OE_PROJECT_STILL_DEFAULT_DURATION_US);
  g_assert_cmpint (clip.source_out_us - clip.source_in_us, ==, 5000000);

  /* A video clip follows the same rule: 1 s of source at 2 s in. */
  g_assert_true (oe_project_insert_clip (project, 0, 1, 5000000, 2000000, 3000000, NULL));

  clip = get_clip (project, 0, 1);

  g_assert_cmpint (clip.source_out_us - clip.source_in_us, ==, 1000000);
  g_assert_cmpint (clip.source_in_us, ==, 2000000);

  g_clear_object (&project);
}

/* --- move and remove ------------------------------------------------------- */

static void
test_move_and_remove (void)
{
  OeProject *project = build_populated ();

  g_assert_true (oe_project_insert_clip (project, 0, 1, 0, 0, 1000000, NULL));
  g_assert_true (oe_project_insert_clip (project, 0, 1, 2000000, 0, 1000000, NULL));

  GError *error = NULL;

  /* Moving the second clip onto the first is an overlap... */
  g_assert_false (oe_project_move_clip (project, 0, 1, 500000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP);
  g_clear_error (&error);

  /* Landing exactly on the other clip's span is a genuine overlap. */
  g_assert_false (oe_project_move_clip (project, 0, 1, 0, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP);
  g_clear_error (&error);

  /* ...but the moved clip's own current footprint is not a target: a
   * no-op move must succeed. */
  g_assert_true (oe_project_move_clip (project, 0, 1, 2000000, NULL));

  /* And so must landing adjacent to a neighbor. */
  g_assert_true (oe_project_move_clip (project, 0, 1, 1000000, NULL));

  /* Order is preserved after the move. */
  g_assert_cmpint (get_clip (project, 0, 0).position_us, ==, 0);
  g_assert_cmpint (get_clip (project, 0, 1).position_us, ==, 1000000);

  /* Bad indices are typed errors. */
  g_assert_false (oe_project_move_clip (project, 0, 9, 0, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP);
  g_clear_error (&error);

  g_assert_false (oe_project_move_clip (project, 9, 0, 0, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK);
  g_clear_error (&error);

  /* Removal: gaps are absence — nothing fills in, neighbors keep
   * their positions. */
  g_assert_true (oe_project_remove_clip (project, 0, 0, NULL));

  OeSequence sequence;

  oe_project_get_sequence (project, &sequence);

  g_assert_cmpuint (((OeTrack *) g_ptr_array_index (sequence.tracks, 0))->clips->len, ==, 1);
  g_assert_cmpint (get_clip (project, 0, 0).position_us, ==, 1000000);
  oe_sequence_clear (&sequence);

  g_assert_false (oe_project_remove_clip (project, 0, 5, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP);
  g_clear_error (&error);

  g_clear_object (&project);
}

/* --- observer contract ------------------------------------------------------ */

static void
test_observer_once (void)
{
  OeProject *project = oe_project_new_default ();
  Observer *observer = observer_new ();

  oe_project_set_observer (project, observer_count, observer);

  oe_project_add_track (project, OE_TRACK_VIDEO);
  g_assert_cmpuint (observer->count, ==, 1);

  oe_project_add_media (project, "/media/a.mp4");
  g_assert_cmpuint (observer->count, ==, 2);

  g_assert_true (oe_project_insert_clip (project, 0, 1, 0, 0, 1000, NULL));
  g_assert_cmpuint (observer->count, ==, 3);

  oe_project_set_name (project, "Renamed");
  g_assert_cmpuint (observer->count, ==, 4);

  g_assert_true (oe_project_move_clip (project, 0, 0, 1000, NULL));
  g_assert_cmpuint (observer->count, ==, 5);

  g_assert_true (oe_project_remove_clip (project, 0, 0, NULL));
  g_assert_cmpuint (observer->count, ==, 6);

  /* Failed mutations never notify: an empty source range and an
   * unknown media ref are both rejected. */
  g_assert_false (oe_project_insert_clip (project, 0, 1, 0, 0, 0, NULL));
  g_assert_cmpuint (observer->count, ==, 6);

  g_assert_false (oe_project_insert_clip (project, 0, 99, 0, 0, 1000, NULL));
  g_assert_cmpuint (observer->count, ==, 6);

  /* ...and teardown stays silent (destruction order contract). */
  g_clear_object (&project);
  g_assert_cmpuint (observer->count, ==, 6);

  g_free (observer);
}

/* --- deep-copy getters ------------------------------------------------------- */

static void
test_deep_copy_getters (void)
{
  OeProject *project = build_populated ();

  g_assert_true (oe_project_insert_clip (project, 0, 1, 0, 0, 1000000, NULL));

  OeSequence sequence;

  oe_project_get_sequence (project, &sequence);

  g_assert_cmpuint (sequence.tracks->len, ==, 2);

  /* Mutating the project never changes the copy... */
  g_assert_true (oe_project_remove_clip (project, 0, 0, NULL));
  oe_project_set_name (project, "Changed");

  g_assert_cmpuint (((OeTrack *) g_ptr_array_index (sequence.tracks, 0))->clips->len, ==, 1);

  OeClip clip = *(OeClip *) g_ptr_array_index (
      ((OeTrack *) g_ptr_array_index (sequence.tracks, 0))->clips, 0);

  g_assert_cmpint (clip.position_us, ==, 0);

  /* ...and freeing the copy never disturbs the project. */
  oe_sequence_clear (&sequence);

  g_assert_true (oe_project_insert_clip (project, 0, 1, 5000000, 0, 1000000, NULL));
  g_assert_cmpstr (oe_project_get_name (project), ==, "Changed");

  g_clear_object (&project);
}

/* --- media references --------------------------------------------------------- */

static void
test_media_refs (void)
{
  OeProject *project = oe_project_new_default ();

  /* Refs start at 1 and increment. */
  g_assert_cmpuint (oe_project_add_media (project, "/media/a.mp4"), ==, 1);
  g_assert_cmpuint (oe_project_add_media (project, "/media/b.wav"), ==, 2);

  guint ref = 0;
  gchar *path = NULL;

  g_assert_true (oe_project_get_media (project, 1, &ref, &path));
  g_assert_cmpuint (ref, ==, 2);
  g_assert_cmpstr (path, ==, "/media/b.wav");
  g_free (path);

  g_assert_true (oe_project_get_media (project, 0, NULL, NULL));

  /* dup_media_path looks up by reference number, not position. */
  gchar *dup = oe_project_dup_media_path (project, 2);

  g_assert_cmpstr (dup, ==, "/media/b.wav");
  g_free (dup);

  g_assert_null (oe_project_dup_media_path (project, 99));

  /* Loader-side explicit refs preserve document numbering... */
  GError *error = NULL;

  g_assert_true (oe_project_add_media_ref (project, 7, "/media/c.png", &error));
  g_assert_no_error (error);

  /* ...and reject duplicates with a typed error. */
  g_assert_false (oe_project_add_media_ref (project, 1, "/media/d.mp4", &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_DUPLICATE_REF);
  g_clear_error (&error);

  /* Clips must name a known ref. */
  oe_project_add_track (project, OE_TRACK_VIDEO);

  g_assert_false (oe_project_insert_clip (project, 0, 99, 0, 0, 1000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_UNKNOWN_MEDIA);
  g_clear_error (&error);
}

/* --- trims ------------------------------------------------------------------ */

/* One video track, an AV media with a probed 2 s source, an unannotated
 * still media, and three clips: AV at [1 s, 2 s), stills at [3 s, 4 s)
 * and [4 s, 4.5 s). */
static OeProject *
build_trim_fixture (void)
{
  OeProject *project = oe_project_new_default ();

  oe_project_add_media (project, "/media/a.mp4"); /* ref 1: AV, probed below */
  oe_project_add_media (project, "/media/b.png"); /* ref 2: still, never annotated */
  oe_project_add_track (project, OE_TRACK_VIDEO);

  oe_project_set_media_source_duration (project, 1, 2000000);

  gint64 annotated = 12345;

  g_assert_true (oe_project_get_media_source_duration (project, 1, &annotated));
  g_assert_cmpint (annotated, ==, 2000000);
  g_assert_true (oe_project_get_media_source_duration (project, 2, &annotated));
  g_assert_cmpint (annotated, ==, 0); /* unannotated reads as unbounded */
  g_assert_false (oe_project_get_media_source_duration (project, 99, NULL));

  g_assert_true (oe_project_insert_clip (project, 0, 1, 1000000, 0, 1000000, NULL));
  g_assert_true (oe_project_insert_clip (project, 0, 2, 3000000, 0, 1000000, NULL));
  g_assert_true (oe_project_insert_clip (project, 0, 2, 4000000, 0, 500000, NULL));

  return project;
}

static void
test_trim (void)
{
  OeProject *project = build_trim_fixture ();

  /* Trim the in edge: position untouched, duration follows the range. */
  g_assert_true (oe_project_trim_clip (project, 0, 0, 500000, 1000000, NULL));
  OeClip clip = get_clip (project, 0, 0);

  g_assert_cmpint (clip.source_in_us, ==, 500000);
  g_assert_cmpint (clip.source_out_us, ==, 1000000);
  g_assert_cmpint (clip.position_us, ==, 1000000);

  /* Trim the out edge to exactly the probed bound — inside is legal. */
  g_assert_true (oe_project_trim_clip (project, 0, 0, 500000, 2000000, NULL));
  clip = get_clip (project, 0, 0);
  g_assert_cmpint (clip.source_out_us, ==, 2000000);
  g_assert_cmpint (clip.position_us, ==, 1000000);

  /* Adjacency: the trimmed clip may end exactly where the next starts. */
  g_assert_true (oe_project_trim_clip (project, 0, 1, 0, 1000000, NULL));
  clip = get_clip (project, 0, 1);
  g_assert_cmpint (clip.source_out_us, ==, 1000000);

  g_clear_object (&project);
}

static void
test_trim_rejections (void)
{
  OeProject *project = build_trim_fixture ();
  Observer *observer = observer_new ();

  oe_project_set_observer (project, observer_count, observer);

  GError *error = NULL;

  /* Empty range, reversed range, negative in. */
  g_assert_false (oe_project_trim_clip (project, 0, 0, 500000, 500000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE);
  g_clear_error (&error);

  g_assert_false (oe_project_trim_clip (project, 0, 0, 900000, 100000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE);
  g_clear_error (&error);

  g_assert_false (oe_project_trim_clip (project, 0, 0, -1, 500000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE);
  g_clear_error (&error);

  /* AV media is bounded by its probed 2 s source. */
  g_assert_false (oe_project_trim_clip (project, 0, 0, 0, 2500000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE);
  g_clear_error (&error);

  /* Bad indices. */
  g_assert_false (oe_project_trim_clip (project, 5, 0, 0, 500000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK);
  g_clear_error (&error);

  g_assert_false (oe_project_trim_clip (project, 0, 9, 0, 500000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP);
  g_clear_error (&error);

  /* Every rejection stayed silent. */
  g_assert_cmpuint (observer->count, ==, 0);

  g_free (observer);
  g_clear_object (&project);
}

static void
test_trim_still_extension (void)
{
  OeProject *project = build_trim_fixture ();

  /* A still extends freely: its source range encodes screen duration,
   * there is no probed bound to exceed (uniform-duration rule). */
  g_assert_true (oe_project_trim_clip (project, 0, 1, 0, 900000, NULL));
  OeClip clip = get_clip (project, 0, 1);

  g_assert_cmpint (clip.source_out_us, ==, 900000);
  g_assert_cmpint (clip.position_us, ==, 3000000);

  /* ...but a still grown across a neighbour is an overlap like any
   * other duration edit — the model invariant stays absolute. */
  GError *error = NULL;

  g_assert_false (oe_project_trim_clip (project, 0, 1, 0, 1500000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP);
  g_clear_error (&error);

  clip = get_clip (project, 0, 1);
  g_assert_cmpint (clip.source_out_us, ==, 900000); /* untouched by the rejection */

  g_clear_object (&project);
}

static void
test_trim_observer (void)
{
  OeProject *project = build_trim_fixture ();
  Observer *observer = observer_new ();

  oe_project_set_observer (project, observer_count, observer);

  g_assert_true (oe_project_trim_clip (project, 0, 0, 250000, 750000, NULL));
  g_assert_cmpuint (observer->count, ==, 1);

  g_assert_true (oe_project_trim_clip (project, 0, 0, 250000, 500000, NULL));
  g_assert_cmpuint (observer->count, ==, 2);

  /* Rejections never notify. */
  g_assert_false (oe_project_trim_clip (project, 0, 0, 500000, 500000, NULL));
  g_assert_cmpuint (observer->count, ==, 2);

  g_free (observer);
  g_clear_object (&project);
}

/* --- destruction order ---------------------------------------------------------- */

static void
test_destruction_order (void)
{
  OeProject *project = oe_project_new_default ();
  Observer *observer = observer_new ();

  oe_project_set_observer (project, observer_count, observer);

  oe_project_add_track (project, OE_TRACK_AUDIO);

  g_assert_cmpuint (observer->count, ==, 1);

  /* The final unref runs dispose: the observer must not fire there. */
  g_clear_object (&project);

  g_assert_cmpuint (observer->count, ==, 1);

  g_free (observer);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/project/defaults", test_defaults);
  g_test_add_func ("/project/track-ordering", test_track_ordering);
  g_test_add_func ("/project/insert-sorted", test_insert_sorted);
  g_test_add_func ("/project/overlap-rejection", test_overlap_rejection);
  g_test_add_func ("/project/clip-duration-rule", test_clip_duration_rule);
  g_test_add_func ("/project/move-and-remove", test_move_and_remove);
  g_test_add_func ("/project/observer-once", test_observer_once);
  g_test_add_func ("/project/deep-copy-getters", test_deep_copy_getters);
  g_test_add_func ("/project/media-refs", test_media_refs);
  g_test_add_func ("/project/trim", test_trim);
  g_test_add_func ("/project/trim-rejections", test_trim_rejections);
  g_test_add_func ("/project/trim-still-extension", test_trim_still_extension);
  g_test_add_func ("/project/trim-observer", test_trim_observer);
  g_test_add_func ("/project/destruction-order", test_destruction_order);

  return g_test_run ();
}
