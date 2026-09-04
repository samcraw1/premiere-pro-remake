/* test_project_format.c — GTK-free tests for JSON v1 project files (Phase 3).
 *
 *   /format/round-trip            save -> load -> save is byte-identical
 *                                 and every field survives.
 *   /format/integer-only          serialization emits num/den integers
 *                                 (never floats); float tokens in input
 *                                 are rejected with a typed error.
 *   /format/missing-member        absent required members fail typed.
 *   /format/unknown-member        schema-foreign members fail typed, at
 *                                 depth too (v1 is closed).
 *   /format/corrupt-json          unparseable files fail with SYNTAX.
 *   /format/newer-version         format-version > 1 fails with VERSION.
 *   /format/bad-values            out-of-domain values fail with VALUE.
 *   /format/atomic-failure        a failed save leaves any pre-existing
 *                                 file byte-identical and no residue.
 *   /format/sequence-size         doc-level width/height written always;
 *                                 absent fields backfill defaults, bad
 *                                 values fail typed (additive v1).
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "../src/core/oe_project.h"
#include "../src/core/oe_project_format.h"

typedef struct
{
  gchar *dir; /* scratch directory, removed by the teardown */
} FormatFixture;

static void
fixture_setup (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->dir = g_dir_make_tmp ("oe-format-test-XXXXXX", NULL);
  g_assert_nonnull (fx->dir);
}

static void
fixture_teardown (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  /* Restore writability in case a test left the directory read-only. */
  g_chmod (fx->dir, 0700);
  g_assert_true (g_rmdir (fx->dir));
  g_free (fx->dir);
}

/* Builds a minimal-but-complete project: NTSC rate, one media, one
 * video and one audio track with adjacent clips. */
static OeProject *
build_round_trip_project (void)
{
  GError *error = NULL;
  OeRational ntsc = oe_time_rate (30000, 1001, &error);

  g_assert_no_error (error);

  OeProject *project = oe_project_new (ntsc);

  oe_project_set_name (project, "Round Trip");
  oe_project_add_media (project, "/media/a.mp4");
  oe_project_add_media (project, "/media/b.wav");
  oe_project_add_track (project, OE_TRACK_VIDEO);
  oe_project_add_track (project, OE_TRACK_AUDIO);

  guint video = 0;
  guint audio = 1;

  /* NTSC frame boundaries: 30 frames = 1001 ms, plus a still image at
   * its default 5 s screen duration. */
  g_assert_true (oe_project_insert_clip (project, video, 1, 0, 0, 1001000, NULL));
  g_assert_true (oe_project_insert_clip (project, video, 1, 1001000, 500000, 1500000, NULL));
  g_assert_true (oe_project_insert_clip (project, video, 1, 2002000, 0,
                                         OE_PROJECT_STILL_DEFAULT_DURATION_US, NULL));
  g_assert_true (oe_project_insert_clip (project, audio, 2, 0, 0, 500000, NULL));

  return project;
}

/* Canonical v1 document template for strict-parse failures. */
static gchar *
write_doc (FormatFixture *fx, const gchar *body)
{
  gchar *path = g_build_filename (fx->dir, "doc.oe", NULL);

  g_assert_true (g_file_set_contents (path, body, -1, NULL));
  return path;
}

static const gchar *MINIMAL_DOC
    = "{\n"
      "  \"obvious-edit-project\": {\n"
      "    \"format-version\": 1,\n"
      "    \"name\": \"Doc\",\n"
      "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
      "    \"media\": [ { \"ref\": 1, \"path\": \"/media/a.mp4\" } ],\n"
      "    \"tracks\": [\n"
      "      { \"kind\": \"video\", \"clips\": [\n"
      "        { \"media-ref\": 1, \"position-us\": 0, \"source-in-us\": 0,"
      " \"source-out-us\": 1000 } ] }\n"
      "    ]\n"
      "  }\n"
      "}\n";

static gchar *
read_all (const gchar *path, gsize *length)
{
  gchar *contents = NULL;

  g_assert_true (g_file_get_contents (path, &contents, length, NULL));
  return contents;
}

/* Fetches the clip at (track, clip) from a fresh deep copy. */
static OeClip
get_clip_at (OeProject *project, guint track, guint clip)
{
  OeSequence sequence;

  oe_project_get_sequence (project, &sequence);
  OeClip result = *(OeClip *) g_ptr_array_index (
      ((OeTrack *) g_ptr_array_index (sequence.tracks, track))->clips, clip);
  oe_sequence_clear (&sequence);
  return result;
}

/* Positive control: the canonical minimal document loads cleanly. */
static void
test_minimal_doc (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  gchar *path = write_doc (fx, MINIMAL_DOC);

  OeProject *loaded = oe_project_format_load (path, &error);

  g_assert_no_error (error);
  g_assert_nonnull (loaded);

  g_assert_cmpstr (oe_project_get_name (loaded), ==, "Doc");
  g_assert_cmpuint (oe_project_get_track_count (loaded), ==, 1);

  OeClip clip = get_clip_at (loaded, 0, 0);

  g_assert_cmpuint (clip.media_ref, ==, 1);
  g_assert_cmpint (clip.source_out_us, ==, 1000);

  g_free (path);
  g_clear_object (&loaded);
}

/* --- round trip -------------------------------------------------------- */

static void
test_round_trip (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *path1 = g_build_filename (fx->dir, "one.oe", NULL);
  gchar *path2 = g_build_filename (fx->dir, "two.oe", NULL);
  GError *error = NULL;

  OeProject *project = build_round_trip_project ();

  g_assert_true (oe_project_format_save (project, path1, &error));
  g_assert_no_error (error);

  OeProject *loaded = oe_project_format_load (path1, &error);

  g_assert_no_error (error);
  g_assert_nonnull (loaded);

  /* Every field survives the trip. */
  g_assert_cmpstr (oe_project_get_name (loaded), ==, "Round Trip");
  g_assert_cmpuint (oe_project_get_track_count (loaded), ==, 2);
  g_assert_cmpuint (oe_project_get_media_count (loaded), ==, 2);

  OeSequence sequence;

  oe_project_get_sequence (loaded, &sequence);

  g_assert_cmpint (sequence.frame_rate.num, ==, 30000);
  g_assert_cmpint (sequence.frame_rate.den, ==, 1001);

  OeTrack *track0 = g_ptr_array_index (sequence.tracks, 0);

  g_assert_cmpint (track0->kind, ==, OE_TRACK_VIDEO);
  g_assert_cmpuint (track0->clips->len, ==, 3);

  OeClip *still = g_ptr_array_index (track0->clips, 2);

  g_assert_cmpint (still->position_us, ==, 2002000);
  g_assert_cmpint (still->source_out_us - still->source_in_us, ==,
                   OE_PROJECT_STILL_DEFAULT_DURATION_US);

  OeTrack *track1 = g_ptr_array_index (sequence.tracks, 1);

  g_assert_cmpint (track1->kind, ==, OE_TRACK_AUDIO);
  g_assert_cmpuint (track1->clips->len, ==, 1);

  guint ref = 0;
  gchar *media_path = NULL;

  g_assert_true (oe_project_get_media (loaded, 1, &ref, &media_path));
  g_assert_cmpuint (ref, ==, 2); /* document numbering is preserved */
  g_assert_cmpstr (media_path, ==, "/media/b.wav");
  g_free (media_path);

  oe_sequence_clear (&sequence);

  /* Save the loaded copy again: the two files must be byte-identical. */
  g_assert_true (oe_project_format_save (loaded, path2, &error));
  g_assert_no_error (error);

  gsize len1 = 0;
  gsize len2 = 0;
  gchar *bytes1 = read_all (path1, &len1);
  gchar *bytes2 = read_all (path2, &len2);

  g_assert_cmpuint (len1, ==, len2);
  g_assert_cmpmem (bytes1, len1, bytes2, len2);

  g_free (bytes1);
  g_free (bytes2);
  g_free (path1);
  g_free (path2);
  g_clear_object (&loaded);
  g_clear_object (&project);
}

/* --- integer-only serialization ------------------------------------------ */

static void
test_integer_only (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *path = g_build_filename (fx->dir, "ints.oe", NULL);
  GError *error = NULL;

  OeProject *project = build_round_trip_project ();

  g_assert_true (oe_project_format_save (project, path, &error));
  g_assert_no_error (error);

  /* The rate is emitted as its num/den pair: a float-serializing
   * writer would have produced 29.97..., never 30000 and 1001. */
  gsize length = 0;
  gchar *text = read_all (path, &length);

  g_assert_nonnull (strstr (text, "\"num\": 30000"));
  g_assert_nonnull (strstr (text, "\"den\": 1001"));
  g_assert_nonnull (strstr (text, "\"format-version\": 1"));
  g_assert_nonnull (strstr (text, "\"position-us\": 2002000"));

  /* Microsecond values never appear in float form either. */
  g_assert_null (strstr (text, "33366.67"));
  g_free (text);

  /* Float tokens on input are rejected (integer-only schema). */
  gchar *float_doc
      = write_doc (fx, "{\n"
                       "  \"obvious-edit-project\": {\n"
                       "    \"format-version\": 1,\n"
                       "    \"name\": \"Doc\",\n"
                       "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                       "    \"media\": [],\n"
                       "    \"tracks\": [\n"
                       "      { \"kind\": \"video\", \"clips\": [\n"
                       "        { \"media-ref\": 0, \"position-us\": 1.5, \"source-in-us\": 0,"
                       " \"source-out-us\": 1000 } ] }\n"
                       "    ]\n"
                       "  }\n"
                       "}\n");

  OeProject *loaded = oe_project_format_load (float_doc, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE);
  g_clear_error (&error);

  g_free (float_doc);
  g_free (path);
  g_clear_object (&project);
}

/* --- missing required members --------------------------------------------- */

static void
test_missing_member (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  /* No frame-rate. */
  gchar *no_rate = write_doc (fx, "{\n"
                                  "  \"obvious-edit-project\": {\n"
                                  "    \"format-version\": 1,\n"
                                  "    \"name\": \"Doc\",\n"
                                  "    \"media\": [],\n"
                                  "    \"tracks\": []\n"
                                  "  }\n"
                                  "}\n");

  OeProject *loaded = oe_project_format_load (no_rate, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_MISSING);
  g_clear_error (&error);
  g_free (no_rate);

  /* No format-version (it is a required member, and first). */
  gchar *no_version = write_doc (fx, "{\n"
                                     "  \"obvious-edit-project\": {\n"
                                     "    \"name\": \"Doc\",\n"
                                     "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                                     "    \"media\": [],\n"
                                     "    \"tracks\": []\n"
                                     "  }\n"
                                     "}\n");

  loaded = oe_project_format_load (no_version, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_MISSING);
  g_clear_error (&error);
  g_free (no_version);

  /* A missing clip member fails too. */
  gchar *no_clip_member
      = write_doc (fx, "{\n"
                       "  \"obvious-edit-project\": {\n"
                       "    \"format-version\": 1,\n"
                       "    \"name\": \"Doc\",\n"
                       "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                       "    \"media\": [],\n"
                       "    \"tracks\": [\n"
                       "      { \"kind\": \"video\", \"clips\": [\n"
                       "        { \"media-ref\": 0, \"position-us\": 0, \"source-in-us\": 0 } ] }\n"
                       "    ]\n"
                       "  }\n"
                       "}\n");

  loaded = oe_project_format_load (no_clip_member, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_MISSING);
  g_clear_error (&error);
  g_free (no_clip_member);

  /* A failed load never hands back a half-built project. */
  g_assert_null (oe_project_format_load ("/nonexistent/path.oe", &error));
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_IO);
  g_clear_error (&error);
}

/* --- unknown members are closed out ----------------------------------------- */

static void
test_unknown_member (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  /* Unknown member at the project level... */
  gchar *top = write_doc (fx, "{\n"
                              "  \"obvious-edit-project\": {\n"
                              "    \"format-version\": 1,\n"
                              "    \"name\": \"Doc\",\n"
                              "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                              "    \"media\": [],\n"
                              "    \"tracks\": [],\n"
                              "    \"future-member\": true\n"
                              "  }\n"
                              "}\n");

  OeProject *loaded = oe_project_format_load (top, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_UNKNOWN_MEMBER);
  g_clear_error (&error);
  g_free (top);

  /* ...and inside a clip: unknown members would be silently dropped on
   * re-save, so no depth tolerates them. */
  gchar *nested
      = write_doc (fx, "{\n"
                       "  \"obvious-edit-project\": {\n"
                       "    \"format-version\": 1,\n"
                       "    \"name\": \"Doc\",\n"
                       "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                       "    \"media\": [],\n"
                       "    \"tracks\": [\n"
                       "      { \"kind\": \"video\", \"clips\": [\n"
                       "        { \"media-ref\": 0, \"position-us\": 0, \"source-in-us\": 0,"
                       " \"source-out-us\": 1000, \"note\": \"x\" } ] }\n"
                       "    ]\n"
                       "  }\n"
                       "}\n");

  loaded = oe_project_format_load (nested, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_UNKNOWN_MEMBER);
  g_clear_error (&error);
  g_free (nested);
}

/* --- corrupt JSON --------------------------------------------------------------- */

static void
test_corrupt_json (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  gchar *truncated = write_doc (fx, "{\n"
                                    "  \"obvious-edit-project\": {\n"
                                    "    \"format-version\": 1,\n"
                                    "    \"name\": \"Doc\"");

  OeProject *loaded = oe_project_format_load (truncated, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_SYNTAX);
  g_clear_error (&error);
  g_free (truncated);

  gchar *garbage = write_doc (fx, "this is not json at all\n");

  loaded = oe_project_format_load (garbage, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_SYNTAX);
  g_clear_error (&error);
  g_free (garbage);
}

/* --- newer version rejection ------------------------------------------------------ */

static void
test_newer_version (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  gchar *v2 = write_doc (fx, "{\n"
                             "  \"obvious-edit-project\": {\n"
                             "    \"format-version\": 2,\n"
                             "    \"name\": \"Doc\",\n"
                             "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                             "    \"media\": [],\n"
                             "    \"tracks\": []\n"
                             "  }\n"
                             "}\n");

  OeProject *loaded = oe_project_format_load (v2, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VERSION);
  g_clear_error (&error);
  g_free (v2);

  /* A string version is not a version. */
  gchar *bad_version = write_doc (fx, "{\n"
                                      "  \"obvious-edit-project\": {\n"
                                      "    \"format-version\": \"1\",\n"
                                      "    \"name\": \"Doc\",\n"
                                      "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                                      "    \"media\": [],\n"
                                      "    \"tracks\": []\n"
                                      "  }\n"
                                      "}\n");

  loaded = oe_project_format_load (bad_version, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE);
  g_clear_error (&error);
  g_free (bad_version);
}

/* --- out-of-domain values ------------------------------------------------------------ */

static void
test_bad_values (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  /* Zero denominator. */
  gchar *zero_den = write_doc (fx, "{\n"
                                   "  \"obvious-edit-project\": {\n"
                                   "    \"format-version\": 1,\n"
                                   "    \"name\": \"Doc\",\n"
                                   "    \"frame-rate\": { \"num\": 25, \"den\": 0 },\n"
                                   "    \"media\": [],\n"
                                   "    \"tracks\": []\n"
                                   "  }\n"
                                   "}\n");

  OeProject *loaded = oe_project_format_load (zero_den, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE);
  g_clear_error (&error);
  g_free (zero_den);

  /* Unknown kind string. */
  gchar *bad_kind = write_doc (fx, "{\n"
                                   "  \"obvious-edit-project\": {\n"
                                   "    \"format-version\": 1,\n"
                                   "    \"name\": \"Doc\",\n"
                                   "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                                   "    \"media\": [],\n"
                                   "    \"tracks\": [\n"
                                   "      { \"kind\": \"subtitle\", \"clips\": [] }\n"
                                   "    ]\n"
                                   "  }\n"
                                   "}\n");

  loaded = oe_project_format_load (bad_kind, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE);
  g_clear_error (&error);
  g_free (bad_kind);

  /* Empty source range (out must exceed in). */
  gchar *empty_range
      = write_doc (fx, "{\n"
                       "  \"obvious-edit-project\": {\n"
                       "    \"format-version\": 1,\n"
                       "    \"name\": \"Doc\",\n"
                       "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                       "    \"media\": [],\n"
                       "    \"tracks\": [\n"
                       "      { \"kind\": \"video\", \"clips\": [\n"
                       "        { \"media-ref\": 0, \"position-us\": 0, \"source-in-us\": 500,"
                       " \"source-out-us\": 500 } ] }\n"
                       "    ]\n"
                       "  }\n"
                       "}\n");

  loaded = oe_project_format_load (empty_range, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE);
  g_clear_error (&error);
  g_free (empty_range);
}

/* --- atomic failure ------------------------------------------------------------------ */

static void
test_atomic_failure (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  gchar *path = g_build_filename (fx->dir, "precious.oe", NULL);
  GError *error = NULL;

  /* First save succeeds and anchors the file. */
  OeProject *project = build_round_trip_project ();

  g_assert_true (oe_project_format_save (project, path, &error));
  g_assert_no_error (error);

  gsize original_length = 0;
  gchar *original = read_all (path, &original_length);

  /* Change the model so a successful re-save would differ. */
  oe_project_set_name (project, "Changed");

  if (geteuid () == 0)
    {
      /* Root ignores directory permissions; the byte-identity claim is
       * skipped rather than asserted dishonestly. */
      g_test_skip ("byte-identity on failed save needs a non-root filesystem");
      g_free (original);
      g_free (path);
      g_clear_object (&project);
      return;
    }

  /* A read-only target directory makes the temp file (and therefore
   * the save) fail. */
  g_assert_true (g_chmod (fx->dir, 0555) == 0);

  g_assert_false (oe_project_format_save (project, path, &error));
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_IO);
  g_clear_error (&error);

  g_assert_true (g_chmod (fx->dir, 0700) == 0);

  /* The pre-existing file is byte-identical. */
  gsize after_length = 0;
  gchar *after = read_all (path, &after_length);

  g_assert_cmpuint (after_length, ==, original_length);
  g_assert_cmpmem (after, after_length, original, original_length);
  g_free (after);
  g_free (original);

  /* And the failed attempt left no temp residue behind. */
  GDir *entries = g_dir_open (fx->dir, 0, NULL);

  g_assert_nonnull (entries);

  const gchar *name = g_dir_read_name (entries);

  while (name != NULL)
    {
      g_assert_null (strstr (name, ".tmp")); /* no residue */
      name = g_dir_read_name (entries);
    }
  g_dir_close (entries);

  /* Saving into a nonexistent directory fails without creating one. */
  gchar *missing_dir_path = g_build_filename (fx->dir, "no-such-dir", "x.oe", NULL);

  g_assert_false (oe_project_format_save (project, missing_dir_path, &error));
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_IO);
  g_clear_error (&error);
  gchar *missing_dir = g_build_filename (fx->dir, "no-such-dir", NULL);

  g_assert_false (g_file_test (missing_dir, G_FILE_TEST_EXISTS));

  g_free (missing_dir);
  g_free (missing_dir_path);
  g_free (path);
  g_clear_object (&project);
}

/* --- sequence size -------------------------------------------------------------------- */

static void
test_sequence_size (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  /* The size is a model mutation; save emits it as doc-level v1 fields. */
  OeProject *project = build_round_trip_project ();

  g_assert_true (oe_project_set_sequence_size (project, 640, 480, NULL));

  gchar *path = g_build_filename (fx->dir, "sized.oe", NULL);

  g_assert_true (oe_project_format_save (project, path, &error));
  g_assert_no_error (error);

  gsize length = 0;
  gchar *saved = read_all (path, &length);

  g_assert_nonnull (strstr (saved, "\"width\": 640"));
  g_assert_nonnull (strstr (saved, "\"height\": 480"));

  OeProject *loaded = oe_project_format_load (path, &error);

  g_assert_no_error (error);
  g_assert_nonnull (loaded);

  OeSequence sequence;

  oe_project_get_sequence (loaded, &sequence);
  g_assert_cmpint (sequence.width, ==, 640);
  g_assert_cmpint (sequence.height, ==, 480);
  oe_sequence_clear (&sequence);

  g_free (saved);
  g_free (path);
  g_clear_object (&loaded);
  g_clear_object (&project);
}

static void
test_sequence_size_backfill (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  /* Files written before the size fields existed load with the
   * documented defaults (additive v1, no version bump). */
  gchar *path = write_doc (fx, MINIMAL_DOC);

  OeProject *loaded = oe_project_format_load (path, &error);

  g_assert_no_error (error);
  g_assert_nonnull (loaded);

  OeSequence sequence;

  oe_project_get_sequence (loaded, &sequence);
  g_assert_cmpint (sequence.width, ==, OE_SEQUENCE_DEFAULT_WIDTH);
  g_assert_cmpint (sequence.height, ==, OE_SEQUENCE_DEFAULT_HEIGHT);
  oe_sequence_clear (&sequence);

  g_free (path);
  g_clear_object (&loaded);
}

static void
test_sequence_size_bad_values (FormatFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  static const struct
  {
    const gchar *body;
  } bad[] = {
    { "\"width\": 641,\n    \"height\": 480," },  /* odd width  */
    { "\"width\": 640,\n    \"height\": 481," },  /* odd height */
    { "\"width\": 0,\n    \"height\": 480," },    /* zero       */
    { "\"width\": -640,\n    \"height\": 480," }, /* negative   */
  };

  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      gchar *body = g_strdup_printf ("{\n"
                                     "  \"obvious-edit-project\": {\n"
                                     "    \"format-version\": 1,\n"
                                     "    \"name\": \"Doc\",\n"
                                     "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                                     "    %s\n"
                                     "    \"media\": [],\n"
                                     "    \"tracks\": []\n"
                                     "  }\n"
                                     "}\n",
                                     bad[i].body);
      gchar *path = write_doc (fx, body);

      OeProject *loaded = oe_project_format_load (path, &error);

      g_assert_null (loaded);
      g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE);
      g_clear_error (&error);

      g_free (body);
      g_free (path);
    }

  /* A non-integer width is a type failure like any other. */
  gchar *string_width = write_doc (fx, "{\n"
                                       "  \"obvious-edit-project\": {\n"
                                       "    \"format-version\": 1,\n"
                                       "    \"name\": \"Doc\",\n"
                                       "    \"frame-rate\": { \"num\": 25, \"den\": 1 },\n"
                                       "    \"width\": \"wide\",\n"
                                       "    \"media\": [],\n"
                                       "    \"tracks\": []\n"
                                       "  }\n"
                                       "}\n");

  OeProject *loaded = oe_project_format_load (string_width, &error);

  g_assert_null (loaded);
  g_assert_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE);
  g_clear_error (&error);
  g_free (string_width);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

#define ADD(name, func)                                                                            \
  g_test_add ("/format/" name, FormatFixture, NULL, fixture_setup, func, fixture_teardown)

  ADD ("round-trip", test_round_trip);
  ADD ("minimal-doc", test_minimal_doc);
  ADD ("integer-only", test_integer_only);
  ADD ("missing-member", test_missing_member);
  ADD ("unknown-member", test_unknown_member);
  ADD ("corrupt-json", test_corrupt_json);
  ADD ("newer-version", test_newer_version);
  ADD ("bad-values", test_bad_values);
  ADD ("atomic-failure", test_atomic_failure);
  ADD ("sequence-size", test_sequence_size);
  ADD ("sequence-size-backfill", test_sequence_size_backfill);
  ADD ("sequence-size-bad-values", test_sequence_size_bad_values);

#undef ADD

  return g_test_run ();
}
