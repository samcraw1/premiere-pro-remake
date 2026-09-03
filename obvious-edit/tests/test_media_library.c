/* test_media_library.c — GTK-free tests for session asset records (Phase 2).
 *
 *   /library/add-importing           add() records IMPORTING and notifies.
 *   /library/mark-ok-contract        mark_ok() stores a metadata copy and
 *                                    requires a pending record.
 *   /library/observer-notified       every mutation fires the observer.
 *   /library/monitor-missing         real GFileMonitor round trip: create
 *                                    in tmp, mark OK, delete the file,
 *                                    pump the main context, observe the
 *                                    MISSING transition.
 *   /library/relink-reprobe          relink() re-points the record and
 *                                    returns it to IMPORTING; a fresh
 *                                    mark_ok() makes it OK again.
 *   /library/unsupported-relink      a relinked record can end
 *                                    UNSUPPORTED and stays for another
 *                                    relink attempt.
 *   /library/remove                  remove() drops the record and
 *                                    observers see it gone.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "../src/app/oe_media_library.h"

typedef struct
{
  gchar *dir;        /* temporary directory for created files */
  gchar *media_path; /* a real file inside dir */
  OeMediaLibrary *library;
} LibraryFixture;

/* Observer harness: records notification ids in order. */
typedef struct
{
  GArray *ids; /* guint */
} ObserverLog;

static void
observer_log (guint asset_id, gpointer user_data)
{
  ObserverLog *log = user_data;

  g_array_append_val (log->ids, asset_id);
}

static ObserverLog *
observer_new (void)
{
  ObserverLog *log = g_new0 (ObserverLog, 1);

  log->ids = g_array_new (FALSE, FALSE, sizeof (guint));
  return log;
}

static void
observer_free (ObserverLog *log)
{
  g_array_unref (log->ids);
  g_free (log);
}

static gboolean
observer_saw (const ObserverLog *log, guint id)
{
  for (guint i = 0; i < log->ids->len; i++)
    {
      if (g_array_index (log->ids, guint, i) == id)
        return TRUE;
    }
  return FALSE;
}

/* Pumps the default main context until @predicate is true or the
 * timeout (seconds) expires — file monitors emit asynchronously. */
static gboolean
pump_until (gboolean (*predicate) (gpointer), gpointer user_data, guint timeout_seconds)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) timeout_seconds * G_USEC_PER_SEC;

  while (g_get_monotonic_time () < deadline)
    {
      if (predicate (user_data))
        return TRUE;

      g_main_context_iteration (NULL, TRUE);
    }
  return predicate (user_data);
}

static void
library_fixture_setup (LibraryFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  GError *error = NULL;

  fx->dir = g_dir_make_tmp ("oe-media-library-XXXXXX", &error);
  g_assert_no_error (error);

  fx->media_path = g_build_filename (fx->dir, "clip.txt", NULL);
  g_assert_true (g_file_set_contents (fx->media_path, "watched", -1, &error));
  g_assert_no_error (error);

  fx->library = oe_media_library_new ();
}

static void
library_fixture_teardown (LibraryFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  oe_media_library_free (fx->library);
  g_free (fx->media_path);
  g_rmdir (fx->dir);
  g_free (fx->dir);
}

static void
test_add_importing (LibraryFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  ObserverLog *log = observer_new ();

  oe_media_library_set_observer (fx->library, observer_log, log);

  guint id = oe_media_library_add (fx->library, fx->media_path);

  g_assert_cmpuint (id, !=, 0);
  g_assert_cmpuint (oe_media_library_count (fx->library), ==, 1);
  g_assert_true (observer_saw (log, id));

  OeAssetInfo info;

  oe_asset_info_init (&info);
  g_assert_true (oe_media_library_get (fx->library, id, &info));
  g_assert_cmpuint (info.id, ==, id);
  g_assert_cmpstr (info.path, ==, fx->media_path);
  g_assert_cmpstr (info.name, ==, "clip.txt");
  g_assert_cmpint (info.status, ==, OE_ASSET_STATUS_IMPORTING);
  g_assert_cmpstr (oe_asset_status_get_name (info.status), ==, "Importing");
  oe_asset_info_clear (&info);

  observer_free (log);
}

static void
test_mark_ok_contract (LibraryFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  guint id = oe_media_library_add (fx->library, fx->media_path);

  OeProbeInfo probed;

  oe_probe_info_init (&probed);
  probed.kind = OE_MEDIA_KIND_AUDIO;
  probed.duration_us = 500000;
  probed.sample_rate = 48000;
  probed.channels = 1;
  probed.container_name = g_strdup ("wav");

  g_assert_true (oe_media_library_mark_ok (fx->library, id, &probed));

  /* Metadata is a copy: clearing the caller's record must not matter. */
  oe_probe_info_clear (&probed);

  OeAssetInfo info;

  oe_asset_info_init (&info);
  g_assert_true (oe_media_library_get (fx->library, id, &info));
  g_assert_cmpint (info.status, ==, OE_ASSET_STATUS_OK);
  g_assert_cmpstr (oe_asset_status_get_name (info.status), ==, "OK");
  g_assert_cmpint (info.info.kind, ==, OE_MEDIA_KIND_AUDIO);
  g_assert_cmpint (info.info.duration_us, ==, 500000);
  g_assert_cmpstr (info.info.container_name, ==, "wav");
  oe_asset_info_clear (&info);

  /* A second mark_ok is not a transition (record is no longer pending). */
  OeProbeInfo again;

  oe_probe_info_init (&again);
  g_assert_false (oe_media_library_mark_ok (fx->library, id, &again));
  oe_probe_info_clear (&again);

  /* Unknown ids fail cleanly. */
  g_assert_false (oe_media_library_mark_ok (fx->library, 9999, &again));
  oe_probe_info_clear (&again);
}

static void
test_observer_notified (LibraryFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  ObserverLog *log = observer_new ();

  oe_media_library_set_observer (fx->library, observer_log, log);

  guint id = oe_media_library_add (fx->library, fx->media_path);

  OeProbeInfo probed;

  oe_probe_info_init (&probed);
  g_assert_true (oe_media_library_mark_ok (fx->library, id, &probed));
  oe_probe_info_clear (&probed);

  g_assert_true (oe_media_library_mark_missing (fx->library, id));
  g_assert_true (oe_media_library_relink (fx->library, id, fx->media_path));

  /* One notification per mutation, in order: add, OK, MISSING, relink. */
  g_assert_cmpuint (log->ids->len, ==, 4);
  for (guint i = 0; i < 4; i++)
    g_assert_cmpuint (g_array_index (log->ids, guint, i), ==, id);

  observer_free (log);
}

typedef struct
{
  const LibraryFixture *fx;
  guint id;
  gboolean missing;
} MissingCheck;

static gboolean
status_is_missing (gpointer user_data)
{
  MissingCheck *check = user_data;
  OeAssetInfo info;

  oe_asset_info_init (&info);
  if (!oe_media_library_get (check->fx->library, check->id, &info))
    return FALSE;
  check->missing = info.status == OE_ASSET_STATUS_MISSING;
  oe_asset_info_clear (&info);
  return check->missing;
}

static void
test_monitor_missing (LibraryFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{

  ObserverLog *log = observer_new ();
  oe_media_library_set_observer (fx->library, observer_log, log);

  guint id = oe_media_library_add (fx->library, fx->media_path);

  OeProbeInfo probed;

  oe_probe_info_init (&probed);
  g_assert_true (oe_media_library_mark_ok (fx->library, id, &probed));
  oe_probe_info_clear (&probed);

  /* External deletion behind the monitor's back. */
  g_assert_true (g_remove (fx->media_path) == 0);

  MissingCheck check = { fx, id, FALSE };

  g_assert_true (pump_until (status_is_missing, &check, 10));

  OeAssetInfo info;

  oe_asset_info_init (&info);
  g_assert_true (oe_media_library_get (fx->library, id, &info));
  g_assert_cmpint (info.status, ==, OE_ASSET_STATUS_MISSING);
  g_assert_cmpstr (oe_asset_status_get_name (info.status), ==, "Missing");
  oe_asset_info_clear (&info);
  g_assert_true (observer_saw (log, id));

  observer_free (log);
}

static void
test_relink_reprobe (LibraryFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{

  /* A second real file to relink to. */
  gchar *other = g_build_filename (fx->dir, "other.txt", NULL);
  GError *error = NULL;

  g_assert_true (g_file_set_contents (other, "relinked", -1, &error));
  g_assert_no_error (error);

  guint id = oe_media_library_add (fx->library, fx->media_path);

  OeProbeInfo probed;

  oe_probe_info_init (&probed);
  g_assert_true (oe_media_library_mark_ok (fx->library, id, &probed));
  oe_probe_info_clear (&probed);

  g_assert_true (oe_media_library_relink (fx->library, id, other));

  OeAssetInfo info;

  oe_asset_info_init (&info);
  g_assert_true (oe_media_library_get (fx->library, id, &info));
  g_assert_cmpint (info.status, ==, OE_ASSET_STATUS_IMPORTING);
  g_assert_cmpstr (info.path, ==, other);
  g_assert_cmpstr (info.name, ==, "other.txt");
  oe_asset_info_clear (&info);

  /* The re-probe succeeds: back to OK with fresh metadata. */
  OeProbeInfo again;

  oe_probe_info_init (&again);
  again.kind = OE_MEDIA_KIND_AUDIO;
  again.sample_rate = 44100;
  g_assert_true (oe_media_library_mark_ok (fx->library, id, &again));
  oe_probe_info_clear (&again);

  g_assert_true (oe_media_library_get (fx->library, id, &info));
  g_assert_cmpint (info.status, ==, OE_ASSET_STATUS_OK);
  oe_asset_info_clear (&info);

  /* Relinking an unknown id fails cleanly. */
  g_assert_false (oe_media_library_relink (fx->library, 9999, other));

  g_free (other);
}

static void
test_unsupported_relink (LibraryFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  guint id = oe_media_library_add (fx->library, fx->media_path);

  OeProbeInfo probed;

  oe_probe_info_init (&probed);
  g_assert_true (oe_media_library_mark_ok (fx->library, id, &probed));
  oe_probe_info_clear (&probed);

  /* Relink turned the record pending again; the re-probe rejects it. */
  g_assert_true (oe_media_library_relink (fx->library, id, fx->media_path));
  g_assert_true (oe_media_library_mark_unsupported (fx->library, id));

  OeAssetInfo info;

  oe_asset_info_init (&info);
  g_assert_true (oe_media_library_get (fx->library, id, &info));
  g_assert_cmpint (info.status, ==, OE_ASSET_STATUS_UNSUPPORTED);
  g_assert_cmpstr (oe_asset_status_get_name (info.status), ==, "Unsupported");
  oe_asset_info_clear (&info);

  /* The row stays in the bin for another relink attempt. */
  g_assert_cmpuint (oe_media_library_count (fx->library), ==, 1);
}

static void
test_remove (LibraryFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  ObserverLog *log = observer_new ();

  oe_media_library_set_observer (fx->library, observer_log, log);

  guint id = oe_media_library_add (fx->library, fx->media_path);

  oe_media_library_remove (fx->library, id);

  g_assert_cmpuint (oe_media_library_count (fx->library), ==, 0);

  OeAssetInfo info;

  oe_asset_info_init (&info);
  g_assert_false (oe_media_library_get (fx->library, id, &info));
  oe_asset_info_clear (&info);

  /* Removing an unknown id is a no-op, not a crash. */
  oe_media_library_remove (fx->library, 9999);

  observer_free (log);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/library/add-importing", LibraryFixture, NULL, library_fixture_setup,
              test_add_importing, library_fixture_teardown);
  g_test_add ("/library/mark-ok-contract", LibraryFixture, NULL, library_fixture_setup,
              test_mark_ok_contract, library_fixture_teardown);
  g_test_add ("/library/observer-notified", LibraryFixture, NULL, library_fixture_setup,
              test_observer_notified, library_fixture_teardown);
  g_test_add ("/library/monitor-missing", LibraryFixture, NULL, library_fixture_setup,
              test_monitor_missing, library_fixture_teardown);
  g_test_add ("/library/relink-reprobe", LibraryFixture, NULL, library_fixture_setup,
              test_relink_reprobe, library_fixture_teardown);
  g_test_add ("/library/unsupported-relink", LibraryFixture, NULL, library_fixture_setup,
              test_unsupported_relink, library_fixture_teardown);
  g_test_add ("/library/remove", LibraryFixture, NULL, library_fixture_setup, test_remove,
              library_fixture_teardown);

  return g_test_run ();
}
