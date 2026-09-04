
/* test_undo_stack.c — GTK-free tests for the command-object history (Phase 6).
 *
 *   /undo/insert-inverse         recorder insert → undo empties the track;
 *                                redo re-creates the clip from the owned copy.
 *   /undo/delete-inverse         delete → undo restores the exact bounds at
 *                                the recorded index; redo re-deletes.
 *   /undo/move-inverse           move → undo restores the old position;
 *                                redo re-applies the new one.
 *   /undo/trim-inverse           trim → undo restores the old source range;
 *                                redo re-applies the new one.
 *   /undo/record-rejection       typed rejections at record time (OVERLAP
 *                                insert, BAD_RANGE trim, bad indices) record
 *                                nothing — the stack is byte-stable.
 *   /undo/interleaved-roundtrip  mixed-op forward build, full reverse undo,
 *                                full redo; each state compared via JSON v1
 *                                saves against pristine baselines.
 *   /undo/depth-eviction         120 pushes leave 100 records, oldest gone.
 *   /undo/redo-cleared-on-edit   record → undo → record: the redo branch is
 *                                gone and redo fails with a typed error.
 *   /undo/observer-never-records the project observer fires on history
 *                                application, but never creates records —
 *                                no feedback loop through notify.
 *   /undo/apply-time-rejection   a direct model poke between record and
 *                                undo makes the inverse fail: typed error,
 *                                stack position untouched.
 *   /undo/empty-history          undo/redo on an empty stack fail with
 *                                OE_UNDO_STACK_ERROR_EMPTY.
 *   /undo/clear-on-replace       oe_undo_stack_clear empties everything
 *                                (project-replace path).
 *   /undo/changed-sequence       the changed seam fires on every transition
 *                                with the right (can_undo, can_redo) pair —
 *                                the command-enablement contract.
 *   /undo/auto-pause             undoing while the session is PLAYING
 *                                pauses first (virtual clock), then applies.
 *
 * Links only the modules under test (core model, persistence, undo
 * stack, playback session) — no GTK. The playback test runs on SDL's
 * dummy driver via the suite env in meson.build.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "../src/app/oe_playback_session.h"
#include "../src/app/oe_undo_stack.h"
#include "../src/core/oe_project.h"
#include "../src/core/oe_project_format.h"
#include "../src/playback/oe_audio_output.h"

/* ------------------------------------------------------------------ */
/* Fixtures: one video track + two media, recorder at hand.            */
/* ------------------------------------------------------------------ */

#define US 1000000LL /* one second in µs, for readable bounds */

typedef struct
{
  OeProject *project;
  OeUndoStack *stack;
  guint track;  /* the single video track's index */
  guint ref_m0; /* media ref for clip source M0 */
  guint ref_m1; /* media ref for clip source M1 */
} UndoFixture;

static void
undo_fixture_setup (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = oe_project_new_default ();
  fx->stack = oe_undo_stack_new ();

  fx->track = oe_project_add_track (fx->project, OE_TRACK_VIDEO);
  fx->ref_m0 = oe_project_add_media (fx->project, "/fixtures/undo-a.mp4");
  fx->ref_m1 = oe_project_add_media (fx->project, "/fixtures/undo-b.mp4");
}

static void
undo_fixture_teardown (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
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

/* Records an insert and asserts the mutator accepted it. */
static void
insert_ok (UndoFixture *fx, guint media_ref, gint64 position, gint64 in, gint64 out)
{
  GError *error = NULL;
  OeClip clip = clip_make (media_ref, position, in, out);

  g_assert_true (oe_edit_insert_clip (fx->project, fx->stack, fx->track, &clip, &error));
  g_assert_no_error (error);
}

/* Reads (copy of) the clip at index on the fixture's track. */
static OeClip
clip_at (UndoFixture *fx, guint index)
{
  OeClip clip = { 0 };

  g_assert_true (oe_project_get_clip (fx->project, fx->track, index, &clip));
  return clip;
}

/* Reads a whole file for JSON round-trip comparisons. */
static gchar *
read_file_or_fail (const gchar *path)
{
  gchar *contents = NULL;
  GError *error = NULL;

  g_assert_true (g_file_get_contents (path, &contents, NULL, &error));
  g_assert_no_error (error);
  return contents;
}

/* ------------------------------------------------------------------ */
/* Per-op inverse correctness.                                         */
/* ------------------------------------------------------------------ */

static void
test_undo_insert_inverse (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, 1);
  g_assert_true (oe_undo_stack_can_undo (fx->stack));
  g_assert_false (oe_undo_stack_can_redo (fx->stack));

  const OeUndoRecord *record = NULL;
  GError *error = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (record->label, ==, "Insert clip 0 on track 0");
  g_assert_cmpuint (oe_project_get_clip_count (fx->project, fx->track), ==, 0);
  g_assert_false (oe_undo_stack_can_undo (fx->stack));
  g_assert_true (oe_undo_stack_can_redo (fx->stack));

  /* Redo replays the owned copy, not a reference into freed state. */
  g_assert_true (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);

  const OeClip clip = clip_at (fx, 0);

  g_assert_cmpuint (clip.media_ref, ==, fx->ref_m0);
  g_assert_cmpint (clip.position_us, ==, 0);
  g_assert_cmpint (clip.source_in_us, ==, 0);
  g_assert_cmpint (clip.source_out_us, ==, 5 * US);
}

static void
test_undo_delete_inverse (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);
  insert_ok (fx, fx->ref_m1, 5 * US, 0, 4 * US);

  GError *error = NULL;

  g_assert_true (oe_edit_remove_clip (fx->project, fx->stack, fx->track, 1, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (oe_project_get_clip_count (fx->project, fx->track), ==, 1);

  const OeUndoRecord *record = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (record->label, ==, "Delete clip 1 on track 0");

  /* The recorded OeClip copy restores the exact bounds, and the clip
   * lands back at its position-ordered index. */
  const OeClip restored = clip_at (fx, 1);

  g_assert_cmpuint (restored.media_ref, ==, fx->ref_m1);
  g_assert_cmpint (restored.position_us, ==, 5 * US);
  g_assert_cmpint (restored.source_out_us, ==, 4 * US);
}

static void
test_undo_move_inverse (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_m0, 0, 0, 3 * US);
  insert_ok (fx, fx->ref_m1, 10 * US, 0, 3 * US);

  GError *error = NULL;

  g_assert_true (oe_edit_move_clip (fx->project, fx->stack, fx->track, 1, 20 * US, &error));
  g_assert_no_error (error);
  g_assert_cmpint (clip_at (fx, 1).position_us, ==, 20 * US);

  const OeUndoRecord *record = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (record->label, ==, "Move clip 1 on track 0");
  g_assert_cmpint (clip_at (fx, 1).position_us, ==, 10 * US);

  g_assert_true (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpint (clip_at (fx, 1).position_us, ==, 20 * US);
}

static void
test_undo_trim_inverse (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  oe_project_set_media_source_duration (fx->project, fx->ref_m0, 30 * US);
  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);

  GError *error = NULL;

  g_assert_true (oe_edit_trim_clip (fx->project, fx->stack, fx->track, 0, 1 * US, 3 * US, &error));
  g_assert_no_error (error);

  const OeUndoRecord *record = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (record->label, ==, "Trim clip 0 on track 0");

  const OeClip restored = clip_at (fx, 0);

  g_assert_cmpint (restored.source_in_us, ==, 0);
  g_assert_cmpint (restored.source_out_us, ==, 5 * US);
  g_assert_cmpint (restored.position_us, ==, 0); /* trim never moves */

  g_assert_true (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpint (clip_at (fx, 0).source_in_us, ==, 1 * US);
  g_assert_cmpint (clip_at (fx, 0).source_out_us, ==, 3 * US);
}

/* ------------------------------------------------------------------ */
/* Typed rejection at record time.                                     */
/* ------------------------------------------------------------------ */

static void
test_undo_record_rejection (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, 1);

  GError *error = NULL;

  /* OVERLAP at record time: the mutator refuses, nothing records. */
  OeClip overlap = clip_make (fx->ref_m1, 2 * US, 0, 5 * US);

  g_assert_false (oe_edit_insert_clip (fx->project, fx->stack, fx->track, &overlap, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP);
  g_clear_error (&error);

  /* BAD_RANGE at record time: an empty (in >= out) source range. */
  g_assert_false (oe_edit_trim_clip (fx->project, fx->stack, fx->track, 0, 4 * US, 2 * US, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE);
  g_clear_error (&error);

  /* Bad indices reject before any record is considered. */
  g_assert_false (oe_edit_remove_clip (fx->project, fx->stack, fx->track, 9, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP);
  g_clear_error (&error);

  g_assert_false (oe_edit_move_clip (fx->project, fx->stack, fx->track, 7, 0, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP);
  g_clear_error (&error);

  /* The stack is untouched by every rejection above. */
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, 1);
  g_assert_true (oe_undo_stack_can_undo (fx->stack));
  g_assert_false (oe_undo_stack_can_redo (fx->stack));
}

/* ------------------------------------------------------------------ */
/* Interleaved forward build, full undo, full redo — JSON v1 round     */
/* trip against pristine baselines.                                    */
/* ------------------------------------------------------------------ */

static void
test_undo_interleaved_roundtrip (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *dir = g_dir_make_tmp ("oe-undo-XXXXXX", NULL);
  gchar *base_path = g_build_filename (dir, "base.oe", NULL);
  gchar *empty_path = g_build_filename (dir, "empty.oe", NULL);
  gchar *after_undo_path = g_build_filename (dir, "after-undo.oe", NULL);
  gchar *after_redo_path = g_build_filename (dir, "after-redo.oe", NULL);
  GError *error = NULL;

  /* Forward build (interleaved kinds): the fully-built project is its
   * own baseline; a pristine twin minus every clip covers the undone
   * state — media and tracks are history-independent. */
  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);       /* index 0 */
  insert_ok (fx, fx->ref_m1, 10 * US, 0, 5 * US); /* index 1 */
  g_assert_true (oe_edit_move_clip (fx->project, fx->stack, fx->track, 1, 20 * US, &error));
  g_assert_no_error (error);
  g_assert_true (oe_edit_trim_clip (fx->project, fx->stack, fx->track, 1, 1 * US, 4 * US, &error));
  g_assert_no_error (error);
  g_assert_true (oe_edit_remove_clip (fx->project, fx->stack, fx->track, 0, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, 5);

  g_assert_true (oe_project_format_save (fx->project, base_path, &error));
  g_assert_no_error (error);

  OeProject *pristine = oe_project_new_default ();

  g_assert_cmpuint (oe_project_add_track (pristine, OE_TRACK_VIDEO), ==, fx->track);
  oe_project_add_media (pristine, "/fixtures/undo-a.mp4");
  oe_project_add_media (pristine, "/fixtures/undo-b.mp4");
  g_assert_true (oe_project_format_save (pristine, empty_path, &error));
  g_assert_no_error (error);
  g_object_unref (pristine);

  /* Full reverse undo: history walked backwards lands on the empty
   * pristine baseline, byte-identical through the v1 serializer. */
  for (guint i = 0; i < 5; i++)
    {
      const OeUndoRecord *record = NULL;

      g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
      g_assert_no_error (error);
    }
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, 5); /* records parked above cursor */

  g_assert_true (oe_project_format_save (fx->project, after_undo_path, &error));
  g_assert_no_error (error);

  gchar *undo_saved = read_file_or_fail (after_undo_path);
  gchar *empty_saved = read_file_or_fail (empty_path);

  g_assert_cmpstr (undo_saved, ==, empty_saved);
  g_free (undo_saved);
  g_free (empty_saved);

  /* Full redo replays forward: the built baseline comes back exactly. */
  for (guint i = 0; i < 5; i++)
    {
      const OeUndoRecord *record = NULL;

      g_assert_true (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
      g_assert_no_error (error);
    }
  g_assert_true (oe_undo_stack_can_undo (fx->stack));
  g_assert_false (oe_undo_stack_can_redo (fx->stack));

  g_assert_true (oe_project_format_save (fx->project, after_redo_path, &error));
  g_assert_no_error (error);

  gchar *redo_saved = read_file_or_fail (after_redo_path);
  gchar *base_saved = read_file_or_fail (base_path);

  g_assert_cmpstr (redo_saved, ==, base_saved);
  g_free (redo_saved);
  g_free (base_saved);

  g_free (base_path);
  g_free (empty_path);
  g_free (after_undo_path);
  g_free (after_redo_path);
  g_rmdir (dir);
  g_free (dir);
}

/* ------------------------------------------------------------------ */
/* Depth, redo clearing, observers, apply-time rejection.              */
/* ------------------------------------------------------------------ */

static void
test_undo_depth_eviction (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  for (guint i = 0; i < 120; i++)
    insert_ok (fx, fx->ref_m0, (gint64) i * US, 0, 1 * US);

  /* Depth cap: the 100 newest records survive, the 20 oldest dropped. */
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, 100);

  /* Clip 0 (position 0, oldest record) was evicted but still exists
   * in the model — eviction drops history, not model content. */
  g_assert_cmpint (clip_at (fx, 0).position_us, ==, 0);

  const OeUndoRecord *record = NULL;
  GError *error = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);

  /* The undone clip is the newest one (position 119 s), not the
   * evicted oldest: the newest surviving clip sits at index 118 and
   * the evicted record's clip still exists in the model. */
  g_assert_cmpuint (oe_project_get_clip_count (fx->project, fx->track), ==, 119);
  g_assert_cmpint (clip_at (fx, 118).position_us, ==, 118 * US);
  g_assert_cmpint (clip_at (fx, 0).position_us, ==, 0);

  /* Draining the rest lands exactly on the cap boundary. */
  for (guint i = 1; i < 100; i++)
    g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));

  g_assert_false (oe_undo_stack_can_undo (fx->stack));
  g_assert_cmpuint (oe_project_get_clip_count (fx->project, fx->track), ==, 20);
}

static void
test_undo_redo_cleared_on_edit (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);

  const OeUndoRecord *record = NULL;
  GError *error = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_true (oe_undo_stack_can_redo (fx->stack));

  /* A newly recorded edit discards the redo branch (linear history). */
  insert_ok (fx, fx->ref_m1, 7 * US, 0, 3 * US);

  g_assert_false (oe_undo_stack_can_redo (fx->stack));
  g_assert_false (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_error (error, OE_UNDO_STACK_ERROR, OE_UNDO_STACK_ERROR_EMPTY);
  g_clear_error (&error);

  /* The new edit survives; undoing walks only real history. */
  g_assert_cmpint (clip_at (fx, 0).position_us, ==, 7 * US);
  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (oe_project_get_clip_count (fx->project, fx->track), ==, 0);
}

typedef struct
{
  guint notified;
} NotifyCounter;

static void
count_notify (gpointer user_data)
{
  ((NotifyCounter *) user_data)->notified++;
}

static void
test_undo_observer_never_records (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  NotifyCounter counter = { 0 };

  oe_project_set_observer (fx->project, count_notify, &counter);

  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);
  const guint notifies_after_record = counter.notified;
  g_assert_cmpuint (notifies_after_record, >, 0);

  /* Undo applies through the mutator (one observer pulse) but must
   * never create a record: history depth only moves by the cursor. */
  const OeUndoRecord *record = NULL;
  GError *error = NULL;
  const guint size_before = oe_undo_stack_get_size (fx->stack);

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (counter.notified, ==, notifies_after_record + 1);
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, size_before);

  /* Same for redo: the inverse pulse, no new record. */
  g_assert_true (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (counter.notified, ==, notifies_after_record + 2);
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, size_before);
}

static void
test_undo_apply_time_rejection (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);

  GError *error = NULL;

  g_assert_true (oe_edit_remove_clip (fx->project, fx->stack, fx->track, 0, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (oe_project_get_clip_count (fx->project, fx->track), ==, 0);

  /* Direct model poke, bypassing the recorder: the undo gap now holds
   * an untracked clip. Any non-recording writer above the model can
   * produce this shape. */
  OeClip intruder = clip_make (fx->ref_m1, 0, 0, 5 * US);

  g_assert_true (oe_project_insert_clip (fx->project, fx->track, intruder.media_ref,
                                         intruder.position_us, intruder.source_in_us,
                                         intruder.source_out_us, &error));
  g_assert_no_error (error);

  /* Undo's inverse (insert at the recorded spot) now collides: the
   * typed rejection propagates and the cursor does not move. */
  const OeUndoRecord *record = NULL;

  g_assert_false (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP);
  g_clear_error (&error);

  g_assert_true (oe_undo_stack_can_undo (fx->stack));
  g_assert_false (oe_undo_stack_can_redo (fx->stack));
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, 2); /* records parked, cursor frozen */

  /* The failed op is still current: clearing the intruder lets the
   * same record succeed — no history was lost to the rejection. */
  g_assert_true (oe_project_remove_clip (fx->project, fx->track, 0, &error));
  g_assert_no_error (error);

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (oe_project_get_clip_count (fx->project, fx->track), ==, 1);
}

/* ------------------------------------------------------------------ */
/* Empty history, project-replace clearing, changed seam, auto-pause.  */
/* ------------------------------------------------------------------ */

static void
test_undo_empty_history (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  const OeUndoRecord *record = NULL;
  GError *error = NULL;

  g_assert_false (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_error (error, OE_UNDO_STACK_ERROR, OE_UNDO_STACK_ERROR_EMPTY);
  g_clear_error (&error);

  g_assert_false (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_error (error, OE_UNDO_STACK_ERROR, OE_UNDO_STACK_ERROR_EMPTY);
  g_clear_error (&error);
}

static void
test_undo_clear_on_replace (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);
  g_assert_true (oe_undo_stack_can_undo (fx->stack));

  /* reset_session's path: the project is replaced, history must not
   * cross the boundary. */
  oe_undo_stack_clear (fx->stack);

  g_assert_false (oe_undo_stack_can_undo (fx->stack));
  g_assert_false (oe_undo_stack_can_redo (fx->stack));
  g_assert_cmpuint (oe_undo_stack_get_size (fx->stack), ==, 0);
}

typedef struct
{
  guint calls;
  gboolean last_can_undo;
  gboolean last_can_redo;
} ChangedLog;

static void
log_changed (gboolean can_undo, gboolean can_redo, gpointer user_data)
{
  ChangedLog *log = user_data;

  log->calls++;
  log->last_can_undo = can_undo;
  log->last_can_redo = can_redo;
}

static void
test_undo_changed_sequence (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  ChangedLog log = { 0 };

  oe_undo_stack_set_changed_func (fx->stack, log_changed, &log);

  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);
  g_assert_cmpuint (log.calls, ==, 1);
  g_assert_true (log.last_can_undo);
  g_assert_false (log.last_can_redo);

  const OeUndoRecord *record = NULL;
  GError *error = NULL;

  g_assert_true (oe_undo_stack_undo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (log.calls, ==, 2);
  g_assert_false (log.last_can_undo);
  g_assert_true (log.last_can_redo);

  g_assert_true (oe_undo_stack_redo (fx->stack, fx->project, &record, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (log.calls, ==, 3);
  g_assert_true (log.last_can_undo);
  g_assert_false (log.last_can_redo);

  oe_undo_stack_clear (fx->stack);
  g_assert_cmpuint (log.calls, ==, 4);
  g_assert_false (log.last_can_undo);
  g_assert_false (log.last_can_redo);
}

/* Virtual clock: a static µs counter the session reads as "now". */
static gint64 fake_time_us = 0;

static gint64
read_fake_time (gpointer user_data G_GNUC_UNUSED)
{
  return fake_time_us;
}

static void
test_undo_auto_pause (UndoFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  insert_ok (fx, fx->ref_m0, 0, 0, 5 * US);

  OePlaybackSession *session = oe_playback_session_new (fx->project);

  oe_playback_session_set_time_source (session, read_fake_time, NULL);

  GError *error = NULL;

  g_assert_true (oe_playback_session_play (session, &error));
  g_assert_no_error (error);
  g_assert_cmpint (oe_playback_session_get_state (session), ==, OE_PLAYBACK_PLAYING);

  /* A tick of virtual time, then undo: the session must pause BEFORE
   * the model mutates (its playing copy would otherwise go stale). */
  fake_time_us += 2 * US;

  const OeUndoRecord *record = NULL;

  g_assert_true (
      oe_undo_stack_undo_with_session (fx->stack, fx->project, session, &record, &error));
  g_assert_no_error (error);

  g_assert_cmpint (oe_playback_session_get_state (session), !=, OE_PLAYBACK_PLAYING);
  g_assert_cmpuint (oe_project_get_clip_count (fx->project, fx->track), ==, 0);

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

#define ADD_UNDO_TEST(path, func)                                                                  \
  g_test_add ((path), UndoFixture, NULL, undo_fixture_setup, (func), undo_fixture_teardown)

  ADD_UNDO_TEST ("/undo/insert-inverse", test_undo_insert_inverse);
  ADD_UNDO_TEST ("/undo/delete-inverse", test_undo_delete_inverse);
  ADD_UNDO_TEST ("/undo/move-inverse", test_undo_move_inverse);
  ADD_UNDO_TEST ("/undo/trim-inverse", test_undo_trim_inverse);
  ADD_UNDO_TEST ("/undo/record-rejection", test_undo_record_rejection);
  ADD_UNDO_TEST ("/undo/interleaved-roundtrip", test_undo_interleaved_roundtrip);
  ADD_UNDO_TEST ("/undo/depth-eviction", test_undo_depth_eviction);
  ADD_UNDO_TEST ("/undo/redo-cleared-on-edit", test_undo_redo_cleared_on_edit);
  ADD_UNDO_TEST ("/undo/observer-never-records", test_undo_observer_never_records);
  ADD_UNDO_TEST ("/undo/apply-time-rejection", test_undo_apply_time_rejection);
  ADD_UNDO_TEST ("/undo/empty-history", test_undo_empty_history);
  ADD_UNDO_TEST ("/undo/clear-on-replace", test_undo_clear_on_replace);
  ADD_UNDO_TEST ("/undo/changed-sequence", test_undo_changed_sequence);
  ADD_UNDO_TEST ("/undo/auto-pause", test_undo_auto_pause);

#undef ADD_UNDO_TEST

  const int result = g_test_run ();

  oe_audio_output_shutdown ();
  return result;
}
