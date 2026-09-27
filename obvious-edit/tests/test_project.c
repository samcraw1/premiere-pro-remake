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
 *   /project/sequence-size         new sequences default to 1920x1080;
 *                                  the setter validates and notifies.
 *   /project/sequence-size-rules   zero, negative, or odd sizes fail
 *                                  with a typed error and never notify.
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
 *   /project/split                 the razor tiles the original footprint
 *                                  and source; state rides both halves;
 *                                  fade-in stays left, fade-out right.
 *   /project/split-rejections      edge/outside cuts and bad indices
 *                                  fail typed and silent; one split
 *                                  notifies once.
 *   /project/split-generator       titles split (no media lookup) and
 *                                  keep the [0, duration) range.
 *   /project/split-keyframes       animated runs stay continuous across
 *                                  the cut; a static-sampling run
 *                                  rides left verbatim.
 *   /project/split-transition      cuts inside a live transition window
 *                                  are refused; its edges are fine.
 *   /project/insert-clip-value     full-state insert round-trips and
 *                                  keeps insert's placement rules.
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

  g_clear_object (&project);
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

/* --- sequence size --------------------------------------------------------- */

static void
test_sequence_size (void)
{
  OeProject *project = oe_project_new_default ();
  Observer *observer = observer_new ();

  oe_project_set_observer (project, observer_count, observer);

  /* New sequences default to HD (Phase 8, D3). */
  OeSequence sequence;

  oe_project_get_sequence (project, &sequence);
  g_assert_cmpint (sequence.width, ==, OE_SEQUENCE_DEFAULT_WIDTH);
  g_assert_cmpint (sequence.height, ==, OE_SEQUENCE_DEFAULT_HEIGHT);
  oe_sequence_clear (&sequence);

  g_assert_true (oe_project_set_sequence_size (project, 640, 360, NULL));
  g_assert_cmpuint (observer->count, ==, 1);

  /* The snapshot reflects the change; the setter copied into the model. */
  oe_project_get_sequence (project, &sequence);
  g_assert_cmpint (sequence.width, ==, 640);
  g_assert_cmpint (sequence.height, ==, 360);
  g_assert_cmpuint (sequence.tracks->len, ==, 0);
  oe_sequence_clear (&sequence);

  g_clear_object (&project);
  g_free (observer);
}

static void
test_sequence_size_rejections (void)
{
  OeProject *project = oe_project_new_default ();
  Observer *observer = observer_new ();

  oe_project_set_observer (project, observer_count, observer);

  static const struct
  {
    gint width, height;
  } bad[] = {
    { 0, 1080 },    { 1920, 0 }, { -1, 1080 }, { 1920, -640 }, { 1919, 1080 }, /* odd width  */
    { 1920, 1081 },                                                            /* odd height */
  };

  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      GError *error = NULL;

      g_assert_false (oe_project_set_sequence_size (project, bad[i].width, bad[i].height, &error));
      g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_SIZE);
      g_clear_error (&error);
    }

  /* Failures never notify and never disturb the model. */
  g_assert_cmpuint (observer->count, ==, 0);

  OeSequence sequence;

  oe_project_get_sequence (project, &sequence);
  g_assert_cmpint (sequence.width, ==, OE_SEQUENCE_DEFAULT_WIDTH);
  g_assert_cmpint (sequence.height, ==, OE_SEQUENCE_DEFAULT_HEIGHT);
  oe_sequence_clear (&sequence);

  g_clear_object (&project);
  g_free (observer);
}

/* --- clip visual properties (Phase 9 Wave A) ---------------------------------- */

static void
test_visual_identity_defaults (void)
{
  OeProject *project = oe_project_new_default ();
  guint track = oe_project_add_track (project, OE_TRACK_VIDEO);
  guint ref = oe_project_add_media (project, "/media/a.mp4");

  g_assert_true (oe_project_insert_clip (project, track, ref, 0, 0, 1000000, NULL));

  OeClip clip;

  g_assert_true (oe_project_get_clip (project, track, 0, &clip));

  /* Zero-value equals today's behavior: identity picture geometry, no
   * dormant state (fades at zero, keyframe store absent). */
  g_assert_true (oe_clip_visual_is_default (&clip.visual));
  g_assert_cmpint (clip.visual.pos_x, ==, 0);
  g_assert_cmpint (clip.visual.pos_y, ==, 0);
  g_assert_cmpuint (clip.visual.scale_permille, ==, 1000);
  g_assert_cmpint (clip.visual.rotation_cdeg, ==, 0);
  g_assert_cmpuint (clip.visual.opacity, ==, 255);
  g_assert_cmpuint (clip.visual.crop_l, ==, 0);
  g_assert_cmpuint (clip.visual.crop_t, ==, 0);
  g_assert_cmpuint (clip.visual.crop_r, ==, 0);
  g_assert_cmpuint (clip.visual.crop_b, ==, 0);
  g_assert_cmpint (clip.visual.fade_in_us, ==, 0);
  g_assert_cmpint (clip.visual.fade_out_us, ==, 0);
  g_assert_null (clip.visual.keyframes);

  g_clear_object (&project);
}

static void
test_visual_mutator (void)
{
  OeProject *project = oe_project_new_default ();
  guint track = oe_project_add_track (project, OE_TRACK_VIDEO);
  guint ref = oe_project_add_media (project, "/media/a.mp4");

  g_assert_true (oe_project_insert_clip (project, track, ref, 0, 0, 1000000, NULL));

  Observer *observer = observer_new ();

  oe_project_set_observer (project, observer_count, observer);

  OeClipVisual visual = oe_clip_visual_identity ();

  visual.pos_x = 40;
  visual.pos_y = -12;
  visual.scale_permille = 1250;
  visual.rotation_cdeg = -1500;
  visual.opacity = 128;
  visual.crop_r = 10;
  visual.crop_b = 20;

  GError *error = NULL;

  g_assert_true (oe_project_set_clip_visual (project, track, 0, &visual, &error));
  g_assert_cmpuint (observer->count, ==, 1); /* exactly one notification */

  OeClip clip;

  g_assert_true (oe_project_get_clip (project, track, 0, &clip));
  g_assert_true (oe_clip_visual_equal (&clip.visual, &visual));

  /* Out-of-domain values fail with the typed error, never notify,
   * and leave the stored visual untouched. */
  static const struct
  {
    const gchar *what;
    int scale_permille;
    int rotation_cdeg;
    guint8 opacity;
    int pos_y;
  } bad[] = {
    { "zero scale", 0, 0, 255, 0 },
    { "scale above 32x", 32001, 0, 255, 0 },
    { "rotation below -360", 0, -36001, 255, 0 },
    { "rotation above 360", 0, 36001, 255, 0 },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      OeClipVisual probe = visual;

      probe.scale_permille = (guint) bad[i].scale_permille;
      probe.rotation_cdeg = bad[i].rotation_cdeg;
      probe.opacity = bad[i].opacity;
      probe.pos_y = bad[i].pos_y;

      g_assert_false (oe_project_set_clip_visual (project, track, 0, &probe, &error));
      g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_VISUAL);
      g_clear_error (&error);
    }

  g_assert_cmpuint (observer->count, ==, 1);

  g_assert_true (oe_project_get_clip (project, track, 0, &clip));
  g_assert_true (oe_clip_visual_equal (&clip.visual, &visual));

  /* Bad indices report the clip error without touching the observer. */
  g_assert_false (oe_project_set_clip_visual (project, track, 3, &visual, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP);
  g_clear_error (&error);
  g_assert_cmpuint (observer->count, ==, 1);

  g_clear_object (&project);
  g_free (observer);
}

static void
test_visual_deep_copy (void)
{
  OeProject *project = oe_project_new_default ();
  guint track = oe_project_add_track (project, OE_TRACK_VIDEO);
  guint ref = oe_project_add_media (project, "/media/a.mp4");

  g_assert_true (oe_project_insert_clip (project, track, ref, 0, 0, 1000000, NULL));

  OeClipVisual visual = oe_clip_visual_identity ();

  visual.scale_permille = 2000;
  visual.opacity = 100;

  g_assert_true (oe_project_set_clip_visual (project, track, 0, &visual, NULL));

  /* The getter's copy is independent: further model mutations leave
   * it exactly as read. */
  OeClip held;

  g_assert_true (oe_project_get_clip (project, track, 0, &held));

  OeClipVisual changed = visual;

  changed.scale_permille = 3000;
  g_assert_true (oe_project_set_clip_visual (project, track, 0, &changed, NULL));
  g_assert_true (oe_clip_visual_equal (&held.visual, &visual));

  /* The sequence snapshot deep-copies the visual too, and mutating
   * the snapshot cannot leak back into the model. */
  OeSequence sequence;

  oe_project_get_sequence (project, &sequence);

  OeTrack *snapshot = g_ptr_array_index (sequence.tracks, 0);
  OeClip *snapshot_clip = g_ptr_array_index (snapshot->clips, 0);

  g_assert_true (oe_clip_visual_equal (&snapshot_clip->visual, &changed));
  g_assert_null (snapshot_clip->visual.keyframes);

  snapshot_clip->visual.scale_permille = 4000;

  g_assert_true (oe_project_get_clip (project, track, 0, &held));
  g_assert_true (oe_clip_visual_equal (&held.visual, &changed));

  oe_sequence_clear (&sequence);
  g_clear_object (&project);
}

/* --- Phase 12: razor split ------------------------------------------------ */

#define SEC 1000000LL

/* One video track, one audio track, one media ref (ref 1, 30 s probed)
 * and one clip at 1 s playing source [2 s, 6 s). */
static OeProject *
build_split_fixture (void)
{
  OeProject *project = build_populated ();

  oe_project_set_media_source_duration (project, 1, 30 * SEC);
  g_assert_true (oe_project_insert_clip (project, 0, 1, 1 * SEC, 2 * SEC, 6 * SEC, NULL));
  return project;
}

/* Borrowed view of a live clip: valid until the next mutation. */
static OeClip
peek_clip (OeProject *project, guint track, guint clip)
{
  OeClip out = { 0 };

  g_assert_true (oe_project_get_clip (project, track, clip, &out));
  return out;
}

static void
test_split (void)
{
  OeProject *project = build_split_fixture ();
  OeClipVisual visual = oe_clip_visual_identity ();
  OeClipAudio audio = oe_clip_audio_identity ();

  visual.opacity = 128;
  visual.pos_x = 40;
  visual.fade_in_us = SEC / 2;
  visual.fade_out_us = 3 * SEC;
  audio.gain = 512;
  g_assert_true (oe_project_set_clip_visual (project, 0, 0, &visual, NULL));
  g_assert_true (oe_project_set_clip_audio (project, 0, 0, &audio, NULL));

  /* Cut 1.5 s into the clip. */
  g_assert_true (oe_project_split_clip (project, 0, 0, 2500000, NULL));
  g_assert_cmpuint (oe_project_get_clip_count (project, 0), ==, 2);

  OeClip left = peek_clip (project, 0, 0);
  OeClip right = peek_clip (project, 0, 1);

  /* The halves tile the original footprint and source range. */
  g_assert_cmpint (left.position_us, ==, 1 * SEC);
  g_assert_cmpint (left.source_in_us, ==, 2 * SEC);
  g_assert_cmpint (left.source_out_us, ==, 3500000);
  g_assert_cmpint (right.position_us, ==, 2500000);
  g_assert_cmpint (right.source_in_us, ==, 3500000);
  g_assert_cmpint (right.source_out_us, ==, 6 * SEC);
  g_assert_cmpuint (right.media_ref, ==, 1);
  g_assert_cmpint (right.kind, ==, OE_CLIP_MEDIA);

  /* Static state rides with both halves. */
  g_assert_cmpuint (left.visual.opacity, ==, 128);
  g_assert_cmpuint (right.visual.opacity, ==, 128);
  g_assert_cmpint (right.visual.pos_x, ==, 40);
  g_assert_cmpint (left.audio.gain, ==, 512);
  g_assert_cmpint (right.audio.gain, ==, 512);

  /* The cut is a hard edit: fade-in stays left, fade-out goes right,
   * clamped to the 2.5 s right half. */
  g_assert_cmpuint (left.visual.fade_in_us, ==, SEC / 2);
  g_assert_cmpuint (left.visual.fade_out_us, ==, 0);
  g_assert_cmpuint (right.visual.fade_in_us, ==, 0);
  g_assert_cmpuint (right.visual.fade_out_us, ==, 2500000);

  g_clear_object (&project);
}

static void
test_split_rejections (void)
{
  OeProject *project = build_split_fixture ();
  Observer *observer = observer_new ();
  GError *error = NULL;

  oe_project_set_observer (project, observer_count, observer);

  /* The clip spans [1 s, 5 s): edges and outside cuts are rejected. */
  const gint64 bad_cuts[] = { 0, 1 * SEC, 5 * SEC, 6 * SEC, -1 };

  for (guint i = 0; i < G_N_ELEMENTS (bad_cuts); i++)
    {
      g_assert_false (oe_project_split_clip (project, 0, 0, bad_cuts[i], &error));
      g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE);
      g_clear_error (&error);
    }

  g_assert_false (oe_project_split_clip (project, 9, 0, 2 * SEC, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK);
  g_clear_error (&error);

  g_assert_false (oe_project_split_clip (project, 0, 1, 2 * SEC, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP);
  g_clear_error (&error);

  /* Rejections never notify; one split notifies exactly once. */
  g_assert_cmpuint (observer->count, ==, 0);
  g_assert_cmpuint (oe_project_get_clip_count (project, 0), ==, 1);
  g_assert_true (oe_project_split_clip (project, 0, 0, 2 * SEC, NULL));
  g_assert_cmpuint (observer->count, ==, 1);

  /* The halves split again like any clip. */
  g_assert_true (oe_project_split_clip (project, 0, 1, 3 * SEC, NULL));
  g_assert_cmpuint (oe_project_get_clip_count (project, 0), ==, 3);
  g_assert_cmpint (peek_clip (project, 0, 2).position_us, ==, 3 * SEC);

  oe_project_set_observer (project, NULL, NULL);
  g_clear_object (&project);
  g_free (observer);
}

static void
test_split_generator (void)
{
  OeProject *project = build_populated ();
  OeClipGenerator generator = oe_clip_generator_identity ();

  generator.text = g_strdup ("Hello");
  generator.size_permille = 100;
  g_assert_true (
      oe_project_insert_generator_clip (project, 0, OE_CLIP_TITLE, 0, 4 * SEC, &generator, NULL));
  oe_clip_generator_clear (&generator);

  /* No media lookup: titles split where they could never trim. */
  g_assert_true (oe_project_split_clip (project, 0, 0, 1 * SEC, NULL));

  OeClip left = peek_clip (project, 0, 0);
  OeClip right = peek_clip (project, 0, 1);

  /* Generated clips keep the [0, duration) source convention (D3). */
  g_assert_cmpint (left.source_in_us, ==, 0);
  g_assert_cmpint (left.source_out_us, ==, 1 * SEC);
  g_assert_cmpint (right.position_us, ==, 1 * SEC);
  g_assert_cmpint (right.source_in_us, ==, 0);
  g_assert_cmpint (right.source_out_us, ==, 3 * SEC);
  g_assert_cmpint (right.kind, ==, OE_CLIP_TITLE);
  g_assert_cmpstr (right.generator.text, ==, "Hello");
  g_assert_true (left.generator.text != right.generator.text); /* deep copy */

  g_clear_object (&project);
}

static gint32
sample (const OeClip *clip, OeKeyframeProperty property, gint64 t)
{
  return oe_keyframes_sample (clip->visual.keyframes, property, t, -1);
}

static void
test_split_keyframes (void)
{
  OeProject *project = build_populated ();

  /* A 4 s clip at 0: opacity ramps 0 → 255 over the whole clip, scale
   * animates only after the cut, and pos-x carries a lone key (a run
   * that samples as static). */
  g_assert_true (oe_project_insert_clip (project, 0, 1, 0, 0, 4 * SEC, NULL));
  g_assert_true (oe_project_set_clip_keyframe (project, 0, 0, OE_KEYFRAME_OPACITY, 0, 0, NULL));
  g_assert_true (
      oe_project_set_clip_keyframe (project, 0, 0, OE_KEYFRAME_OPACITY, 4 * SEC, 255, NULL));
  g_assert_true (oe_project_set_clip_keyframe (project, 0, 0, OE_KEYFRAME_SCALE_PERMILLE, 2 * SEC,
                                               1000, NULL));
  g_assert_true (oe_project_set_clip_keyframe (project, 0, 0, OE_KEYFRAME_SCALE_PERMILLE, 3 * SEC,
                                               2000, NULL));
  g_assert_true (
      oe_project_set_clip_keyframe (project, 0, 0, OE_KEYFRAME_POS_X, 3 * SEC, 99, NULL));

  g_assert_true (oe_project_split_clip (project, 0, 0, 1 * SEC, NULL));

  OeClip left = peek_clip (project, 0, 0);
  OeClip right = peek_clip (project, 0, 1);

  /* Opacity: continuous across the cut (255 / 4 = 63.75 → 64). */
  g_assert_cmpuint (oe_keyframes_count_for_property (left.visual.keyframes, OE_KEYFRAME_OPACITY),
                    ==, 2);
  g_assert_cmpint (sample (&left, OE_KEYFRAME_OPACITY, 0), ==, 0);
  g_assert_cmpint (sample (&left, OE_KEYFRAME_OPACITY, 1 * SEC), ==, 64);
  g_assert_cmpint (sample (&right, OE_KEYFRAME_OPACITY, 0), ==, 64);
  g_assert_cmpint (sample (&right, OE_KEYFRAME_OPACITY, 3 * SEC), ==, 255);
  g_assert_cmpint (sample (&right, OE_KEYFRAME_OPACITY, 1 * SEC), ==, 128);

  /* Scale: the left half holds the pre-animation value (two keys, so
   * the run animates instead of degrading to the static field); the
   * right half keeps the ramp rebased by the cut. */
  g_assert_cmpuint (
      oe_keyframes_count_for_property (left.visual.keyframes, OE_KEYFRAME_SCALE_PERMILLE), ==, 2);
  g_assert_cmpint (sample (&left, OE_KEYFRAME_SCALE_PERMILLE, SEC / 2), ==, 1000);
  g_assert_cmpint (sample (&right, OE_KEYFRAME_SCALE_PERMILLE, 1 * SEC), ==, 1000);
  g_assert_cmpint (sample (&right, OE_KEYFRAME_SCALE_PERMILLE, 3 * SEC / 2), ==, 1500);
  g_assert_cmpint (sample (&right, OE_KEYFRAME_SCALE_PERMILLE, 2 * SEC), ==, 2000);

  /* The lone pos-x key never animated: it rides left verbatim, and
   * both halves keep sampling the static field. */
  g_assert_cmpuint (oe_keyframes_count_for_property (left.visual.keyframes, OE_KEYFRAME_POS_X), ==,
                    1);
  g_assert_cmpuint (oe_keyframes_count_for_property (right.visual.keyframes, OE_KEYFRAME_POS_X), ==,
                    0);
  g_assert_cmpint (sample (&left, OE_KEYFRAME_POS_X, 0), ==, -1);
  g_assert_cmpint (sample (&right, OE_KEYFRAME_POS_X, 0), ==, -1);

  /* Every stored key lies inside its half — the keyframe mutator's
   * [0, length] contract holds for both. */
  const OeClip *halves[] = { &left, &right };

  for (guint h = 0; h < 2; h++)
    {
      const GArray *keys = halves[h]->visual.keyframes;
      const gint64 length = halves[h]->source_out_us - halves[h]->source_in_us;

      for (guint i = 0; i < keys->len; i++)
        {
          const OeKeyframe *kf = &g_array_index (keys, OeKeyframe, i);

          if (kf->property != OE_KEYFRAME_POS_X)
            g_assert_cmpint (kf->time_us, <=, length);
        }
    }

  g_clear_object (&project);
}

static void
test_split_transition (void)
{
  OeProject *project = build_populated ();
  GError *error = NULL;

  g_assert_true (oe_project_insert_clip (project, 0, 1, 0, 0, 2 * SEC, NULL));
  g_assert_true (oe_project_insert_clip (project, 0, 1, 2 * SEC, 0, 2 * SEC, NULL));

  const OeTransition t = { 0, 2 * SEC, 1 * SEC, OE_TRANSITION_CROSS_DISSOLVE };

  /* Window [1.5 s, 2.5 s). */
  g_assert_true (oe_project_add_transition (project, &t, NULL));

  /* Inside the window on either side of the boundary: refused. */
  g_assert_false (oe_project_split_clip (project, 0, 0, 1600000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRANSITION);
  g_clear_error (&error);
  g_assert_false (oe_project_split_clip (project, 0, 1, 2400000, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRANSITION);
  g_clear_error (&error);

  /* On the window's edges: allowed, and the transition stays live. */
  g_assert_true (oe_project_split_clip (project, 0, 0, 1500000, NULL));
  g_assert_true (oe_project_split_clip (project, 0, 2, 2500000, NULL));

  OeSequence sequence;

  oe_project_get_sequence (project, &sequence);
  g_assert_cmpuint (sequence.transitions->len, ==, 1);
  g_assert_true (
      oe_transition_window (&sequence, g_ptr_array_index (sequence.transitions, 0)).active);
  oe_sequence_clear (&sequence);

  g_clear_object (&project);
}

static void
test_insert_clip_value (void)
{
  OeProject *project = build_split_fixture ();
  GError *error = NULL;

  g_assert_true (oe_project_set_clip_keyframe (project, 0, 0, OE_KEYFRAME_OPACITY, 0, 10, NULL));
  g_assert_true (
      oe_project_set_clip_keyframe (project, 0, 0, OE_KEYFRAME_OPACITY, 2 * SEC, 200, NULL));

  OeClip value = peek_clip (project, 0, 0);

  /* Stage a private copy, then clear the track and restore it. */
  value.visual.keyframes = oe_keyframes_copy_array (value.visual.keyframes);
  value.audio.pan = 100;
  g_assert_true (oe_project_remove_clip (project, 0, 0, NULL));
  g_assert_true (oe_project_insert_clip_value (project, 0, &value, NULL));

  OeClip back = peek_clip (project, 0, 0);

  g_assert_cmpint (back.position_us, ==, value.position_us);
  g_assert_cmpint (back.source_in_us, ==, value.source_in_us);
  g_assert_cmpint (back.audio.pan, ==, 100);
  g_assert_true (oe_clip_visual_equal (&back.visual, &value.visual));
  g_assert_true (back.visual.keyframes != value.visual.keyframes); /* deep copy */

  /* Placement rules match oe_project_insert_clip. */
  g_assert_false (oe_project_insert_clip_value (project, 0, &value, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP);
  g_clear_error (&error);

  /* Generated clips must carry the [0, duration) range. */
  OeClip solid = value;

  solid.kind = OE_CLIP_SOLID;
  solid.media_ref = 0;
  solid.position_us = 20 * SEC;
  g_assert_false (oe_project_insert_clip_value (project, 0, &solid, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE);
  g_clear_error (&error);

  g_clear_pointer (&value.visual.keyframes, g_array_unref);
  g_clear_object (&project);
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
  g_test_add_func ("/project/sequence-size", test_sequence_size);
  g_test_add_func ("/project/sequence-size-rules", test_sequence_size_rejections);

  g_test_add_func ("/project/observer-once", test_observer_once);
  g_test_add_func ("/project/deep-copy-getters", test_deep_copy_getters);
  g_test_add_func ("/project/media-refs", test_media_refs);
  g_test_add_func ("/project/trim", test_trim);
  g_test_add_func ("/project/trim-rejections", test_trim_rejections);
  g_test_add_func ("/project/trim-still-extension", test_trim_still_extension);
  g_test_add_func ("/project/trim-observer", test_trim_observer);
  g_test_add_func ("/project/visual-identity-defaults", test_visual_identity_defaults);
  g_test_add_func ("/project/visual-mutator", test_visual_mutator);
  g_test_add_func ("/project/visual-deep-copy", test_visual_deep_copy);
  g_test_add_func ("/project/split", test_split);
  g_test_add_func ("/project/split-rejections", test_split_rejections);
  g_test_add_func ("/project/split-generator", test_split_generator);
  g_test_add_func ("/project/split-keyframes", test_split_keyframes);
  g_test_add_func ("/project/split-transition", test_split_transition);
  g_test_add_func ("/project/insert-clip-value", test_insert_clip_value);
  g_test_add_func ("/project/destruction-order", test_destruction_order);

  return g_test_run ();
}
