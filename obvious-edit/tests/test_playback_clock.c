/* test_playback_clock.c — the GTK-free playback session, headless.
 *
 * Covers the Phase 5 clock contract against the real project model:
 * clip→source mapping (topmost track wins, half-open spans, source
 * clamping), deadline math (strictly future, frame-spaced), the
 * stopped/paused/playing state machine (empty timeline, pause/resume
 * drift accounting, stop parking), seek clamping and live seek, and
 * end-of-sequence behavior — all GTK-free.
 *
 * The audio adapter opens SDL's dummy driver (SDL_AUDIODRIVER=dummy in
 * the meson test environment), so the session runs with a real stream
 * object while decode stays deterministic: every clip references media
 * whose file does not exist, exercising only the missing-media
 * continuation path (reported once per run, transport unaffected).
 *
 * Wall-cadence tests install a virtual clock through
 * oe_playback_session_set_time_source(): assertions advance the clock by
 * hand, so they are deterministic and Valgrind-clean — no real sleeps.
 */

#include <glib.h>

#include "../src/app/oe_playback_session.h"
#include "../src/core/oe_project.h"
#include "../src/core/oe_time.h"
#include "../src/playback/oe_audio_output.h"

/* Bogus-but-canonical media path: decode always fails fast (ENOENT). */
#define MISSING_MEDIA_PATH "/nonexistent/oe-phase-5-missing-media.wav"

typedef struct
{
  OePlaybackEvent last_event;
  guint event_count;
  OePlaybackSession *session;
  OeProject *project;
} ClockFixture;

static void
count_event (const OePlaybackSession *session G_GNUC_UNUSED, OePlaybackEvent event,
             const gchar *detail G_GNUC_UNUSED, gpointer user_data)
{
  ClockFixture *fx = user_data;

  fx->last_event = event;
  fx->event_count++;
}

static void
noop_notify (const OePlaybackSession *session G_GNUC_UNUSED, gint64 position G_GNUC_UNUSED,
             OePlaybackState state G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
}

/* Virtual clock: the injected time source for wall-cadence tests. Every
 * timing assertion advances the clock by hand, so runs are deterministic
 * and Valgrind-clean — no real sleeps anywhere in this suite. */
typedef struct
{
  gint64 now_us; /* monotonic-scale virtual "now" */
} FakeClock;

static gint64
fake_time (gpointer data)
{
  return ((FakeClock *) data)->now_us;
}

static void
fake_advance (FakeClock *clock, gint64 us)
{
  clock->now_us += us;
}

static void
clock_fixture_setup (ClockFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->last_event = -1;
  fx->event_count = 0;
  fx->project = NULL;
  fx->session = NULL;
}

static void
clock_fixture_teardown (ClockFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  g_clear_pointer (&fx->session, oe_playback_session_free);
  g_clear_object (&fx->project);
}

/* A 25 fps project with the given number of video tracks, plus one
 * audio track (so the session sees both kinds). */
static OeProject *
build_project (guint video_tracks)
{
  GError *error = NULL;
  OeRational rate = oe_time_rate (25, 1, &error);

  g_assert_no_error (error);

  OeProject *project = oe_project_new (rate);

  for (guint i = 0; i < video_tracks; i++)
    oe_project_add_track (project, OE_TRACK_VIDEO);
  oe_project_add_track (project, OE_TRACK_AUDIO);
  return project;
}

/* A 25 fps project with one video track and one audio track. */
static OeProject *
build_project_25fps (void)
{
  GError *error = NULL;
  OeRational rate = oe_time_rate (25, 1, &error);

  g_assert_no_error (error);

  OeProject *project = oe_project_new (rate);

  oe_project_add_track (project, OE_TRACK_VIDEO);
  oe_project_add_track (project, OE_TRACK_AUDIO);
  return project;
}

/* Media ref for the missing path (decode fails fast; session continues). */
static guint
add_missing_media (OeProject *project)
{
  return oe_project_add_media (project, MISSING_MEDIA_PATH);
}

/* Video clip [position, position + length_us) with the given source
 * range, on the project's single video track. */
static void
add_video_clip (OeProject *project, guint media_ref, gint64 position_us, gint64 source_in_us,
                gint64 source_out_us)
{
  GError *error = NULL;

  g_assert_true (oe_project_insert_clip (project, 0, media_ref, position_us, source_in_us,
                                         source_out_us, &error));
}

/* ------------------------------------------------------------------ */
/* Mapping                                                             */
/* ------------------------------------------------------------------ */

static void
test_mapping_topmost_track_wins (ClockFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  /* Two video tracks: the overlap test needs a genuine topmost. */
  fx->project = build_project (2);
  const guint ref = add_missing_media (fx->project);

  /* Track 0: [0, 1 s) source [10 s, 11 s). Track 1 (topmost): [0.5, 1.5 s)
   * source [20 s, 21 s). */
  add_video_clip (fx->project, ref, 0, G_GINT64_CONSTANT (10000000), G_GINT64_CONSTANT (11000000));
  g_assert_true (oe_project_insert_clip (fx->project, 1, ref, G_GINT64_CONSTANT (500000),
                                         G_GINT64_CONSTANT (20000000), G_GINT64_CONSTANT (21000000),
                                         NULL));

  /* get_sequence replaces zeroed storage wholesale. */
  OeSequence sequence = { 0 };

  oe_project_get_sequence (fx->project, &sequence);

  OePlaybackMapping map;

  /* Inside only the lower clip: track 0 wins, source = in + offset. */
  g_assert_true (oe_playback_session_map (&sequence, OE_TRACK_VIDEO, 250000, &map));
  g_assert_cmpuint (map.track_index, ==, 0);
  g_assert_cmpuint (map.source_us, ==, G_GINT64_CONSTANT (10250000));

  /* Inside both: the topmost (higher-index) track wins. Source is the
   * clip's in-point plus the offset from the CLIP's start (250 000 µs
   * past position 500 000), not the sequence offset. */
  g_assert_true (oe_playback_session_map (&sequence, OE_TRACK_VIDEO, 750000, &map));
  g_assert_cmpuint (map.track_index, ==, 1);
  g_assert_cmpuint (map.source_us, ==, G_GINT64_CONSTANT (20250000));

  /* Half-open: at the topmost clip's end nothing maps (both spans are
   * end-exclusive; the lower clip already ended at 1 s)... */
  g_assert_false (oe_playback_session_map (&sequence, OE_TRACK_VIDEO, 1500000, &map));
  /* ...and one microsecond earlier the topmost clip still wins. */
  g_assert_true (oe_playback_session_map (&sequence, OE_TRACK_VIDEO, 1500000 - 1, &map));
  g_assert_cmpuint (map.track_index, ==, 1);

  /* Wrong kind never maps. */
  g_assert_false (oe_playback_session_map (&sequence, OE_TRACK_AUDIO, 250000, &map));

  oe_sequence_clear (&sequence);
}

static void
test_mapping_source_clamped_into_range (ClockFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = build_project_25fps ();
  const guint ref = add_missing_media (fx->project);

  /* A clip whose source range starts mid-file: [0, 1 s) source [30 s, 31 s). */
  add_video_clip (fx->project, ref, 0, G_GINT64_CONSTANT (30000000), G_GINT64_CONSTANT (31000000));

  OeSequence sequence = { 0 };

  oe_project_get_sequence (fx->project, &sequence);

  OePlaybackMapping map;

  g_assert_true (oe_playback_session_map (&sequence, OE_TRACK_VIDEO, 0, &map));
  g_assert_cmpuint (map.source_us, ==, G_GINT64_CONSTANT (30000000));

  g_assert_true (oe_playback_session_map (&sequence, OE_TRACK_VIDEO, 999999, &map));
  g_assert_cmpuint (map.source_us, ==, G_GINT64_CONSTANT (30999999));

  oe_sequence_clear (&sequence);
}

/* ------------------------------------------------------------------ */
/* Empty timeline                                                      */
/* ------------------------------------------------------------------ */

static void
test_empty_sequence_reports_nothing_to_play (ClockFixture *fx,
                                             gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = oe_project_new_default ();
  fx->session = oe_playback_session_new ((const OeProject *) fx->project);
  oe_playback_session_set_event_func (fx->session, count_event, fx);

  GError *error = NULL;

  g_assert_true (oe_playback_session_play (fx->session, &error));
  g_assert_cmpint (oe_playback_session_get_state (fx->session), ==, OE_PLAYBACK_STOPPED);
  g_assert_cmpuint (fx->event_count, ==, 1);
  g_assert_cmpint (fx->last_event, ==, OE_PLAYBACK_EVENT_NOTHING_TO_PLAY);
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, 0);
}

/* ------------------------------------------------------------------ */
/* Deadline math                                                       */
/* ------------------------------------------------------------------ */

static void
test_tick_deadlines_are_future_and_frame_spaced (ClockFixture *fx,
                                                 gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = build_project_25fps ();
  const guint ref = add_missing_media (fx->project);

  add_video_clip (fx->project, ref, 0, 0, G_GINT64_CONSTANT (5000000)); /* 5 s */

  fx->session = oe_playback_session_new ((const OeProject *) fx->project);
  FakeClock clock = { G_GINT64_CONSTANT (1000000) };
  oe_playback_session_set_time_source (fx->session, fake_time, &clock);
  oe_playback_session_set_event_func (fx->session, count_event, fx);
  oe_playback_session_set_observer (fx->session, noop_notify, fx);

  GError *error = NULL;

  g_assert_true (oe_playback_session_play (fx->session, &error));
  g_assert_cmpint (oe_playback_session_get_state (fx->session), ==, OE_PLAYBACK_PLAYING);

  /* play() anchored the clock at the virtual now; tick() at the anchor
   * hands back exactly one frame — strictly in the future. */
  const gint64 anchor = clock.now_us;
  const gint64 deadline = oe_playback_session_tick (fx->session);

  g_assert_cmpint (deadline, ==, anchor + 40000);
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, 0);

  /* Advance past the first deadline: the anchored clock spaces the next
   * deadline exactly one frame later — 25 fps → 40 000 µs, never
   * drift-accumulated, and independent of scheduling jitter. */
  fake_advance (&clock, 41000);
  const gint64 next_deadline = oe_playback_session_tick (fx->session);

  g_assert_cmpint (next_deadline, ==, anchor + 80000);
  g_assert_cmpint (llabs (next_deadline - deadline - 40000), ==, 0);

  /* Position advanced with the (virtual) clock. */
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, 41000);

  oe_playback_session_stop (fx->session);
}

/* ------------------------------------------------------------------ */
/* End of sequence                                                     */
/* ------------------------------------------------------------------ */

static void
test_end_of_sequence_stops_and_parks (ClockFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = build_project_25fps ();
  const guint ref = add_missing_media (fx->project);

  add_video_clip (fx->project, ref, 0, 0, G_GINT64_CONSTANT (1000000)); /* 1 s */

  fx->session = oe_playback_session_new ((const OeProject *) fx->project);
  FakeClock clock = { 0 };
  oe_playback_session_set_time_source (fx->session, fake_time, &clock);
  oe_playback_session_set_event_func (fx->session, count_event, fx);

  GError *error = NULL;

  g_assert_true (oe_playback_session_play (fx->session, &error));
  g_assert_cmpint (oe_playback_session_get_sequence_end (fx->session), ==,
                   G_GINT64_CONSTANT (1000000));

  /* Advance one frame per tick: 1 s / 40 ms = 25 ticks, then the
   * position crosses the sequence end and the session stops with the
   * playhead parked. The iteration bound is exact — a virtual clock
   * needs no wall-time budget. */
  guint iterations = 0;

  while (oe_playback_session_get_state (fx->session) == OE_PLAYBACK_PLAYING && iterations < 30)
    {
      fake_advance (&clock, 40000);
      oe_playback_session_tick (fx->session);
      iterations++;
    }

  g_assert_cmpuint (iterations, <=, 26);
  g_assert_cmpint (oe_playback_session_get_state (fx->session), ==, OE_PLAYBACK_STOPPED);
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, G_GINT64_CONSTANT (1000000));
  g_assert_cmpuint (fx->event_count, >=, 1);
  g_assert_cmpint (fx->last_event, ==, OE_PLAYBACK_EVENT_END_OF_SEQUENCE);

  /* A tick after stopping is a no-op (and never re-fires the event). */
  const guint events_before = fx->event_count;

  oe_playback_session_tick (fx->session);
  g_assert_cmpuint (fx->event_count, ==, events_before);
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, G_GINT64_CONSTANT (1000000));
}

/* ------------------------------------------------------------------ */
/* Pause / resume drift accounting                                     */
/* ------------------------------------------------------------------ */

static void
test_pause_resume_reanchors (ClockFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = build_project_25fps ();
  const guint ref = add_missing_media (fx->project);

  add_video_clip (fx->project, ref, 0, 0, G_GINT64_CONSTANT (30000000)); /* 30 s */

  fx->session = oe_playback_session_new ((const OeProject *) fx->project);
  FakeClock clock = { 0 };
  oe_playback_session_set_time_source (fx->session, fake_time, &clock);

  GError *error = NULL;

  g_assert_true (oe_playback_session_play (fx->session, &error));

  /* 60 ms of playback, then pause: the parked position is exact. */
  fake_advance (&clock, 60000);
  oe_playback_session_pause (fx->session);

  g_assert_cmpint (oe_playback_session_get_state (fx->session), ==, OE_PLAYBACK_PAUSED);
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, G_GINT64_CONSTANT (60000));

  /* Paused: the position is frozen — clock movement does not creep in. */
  fake_advance (&clock, 80000);
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, G_GINT64_CONSTANT (60000));

  /* Resume: drift accounting resets — the clock re-anchors at the parked
   * position and advances from there, not from the time lost while
   * paused. */
  g_assert_true (oe_playback_session_play (fx->session, &error));
  g_assert_cmpint (oe_playback_session_get_state (fx->session), ==, OE_PLAYBACK_PLAYING);

  fake_advance (&clock, 60000);
  oe_playback_session_pause (fx->session);

  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, G_GINT64_CONSTANT (120000));

  oe_playback_session_stop (fx->session);
}

/* ------------------------------------------------------------------ */
/* Stop parks; play continues from the parked position                 */
/* ------------------------------------------------------------------ */

static void
test_stop_parks_and_play_continues (ClockFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = build_project_25fps ();
  const guint ref = add_missing_media (fx->project);

  add_video_clip (fx->project, ref, 0, 0, G_GINT64_CONSTANT (30000000));

  fx->session = oe_playback_session_new ((const OeProject *) fx->project);
  FakeClock clock = { 0 };
  oe_playback_session_set_time_source (fx->session, fake_time, &clock);

  GError *error = NULL;

  g_assert_true (oe_playback_session_play (fx->session, &error));
  fake_advance (&clock, 60000);
  oe_playback_session_stop (fx->session);

  g_assert_cmpint (oe_playback_session_get_state (fx->session), ==, OE_PLAYBACK_STOPPED);
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, G_GINT64_CONSTANT (60000));

  /* Play from the parked position, not from zero. */
  g_assert_true (oe_playback_session_play (fx->session, &error));
  fake_advance (&clock, 60000);

  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, G_GINT64_CONSTANT (120000));

  oe_playback_session_stop (fx->session);
}

/* ------------------------------------------------------------------ */
/* Seek                                                                */
/* ------------------------------------------------------------------ */

static void
test_seek_clamps (ClockFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = build_project_25fps ();
  const guint ref = add_missing_media (fx->project);

  add_video_clip (fx->project, ref, 0, 0, G_GINT64_CONSTANT (2000000)); /* 2 s */

  fx->session = oe_playback_session_new ((const OeProject *) fx->project);

  oe_playback_session_seek (fx->session, -1000);
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, 0);

  oe_playback_session_seek (fx->session, G_GINT64_CONSTANT (99000000));
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==,
                   oe_playback_session_get_sequence_end (fx->session));

  oe_playback_session_seek (fx->session, G_GINT64_CONSTANT (1000000));
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, G_GINT64_CONSTANT (1000000));
}

static void
test_seek_during_playback_resets_clock (ClockFixture *fx, gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = build_project_25fps ();
  const guint ref = add_missing_media (fx->project);

  add_video_clip (fx->project, ref, 0, 0, G_GINT64_CONSTANT (30000000));

  fx->session = oe_playback_session_new ((const OeProject *) fx->project);
  FakeClock clock = { 0 };
  oe_playback_session_set_time_source (fx->session, fake_time, &clock);

  GError *error = NULL;

  g_assert_true (oe_playback_session_play (fx->session, &error));
  fake_advance (&clock, 50000);

  /* Live seek: the clock re-anchors at the target and keeps playing. */
  oe_playback_session_seek (fx->session, G_GINT64_CONSTANT (5000000));

  g_assert_cmpint (oe_playback_session_get_state (fx->session), ==, OE_PLAYBACK_PLAYING);
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, G_GINT64_CONSTANT (5000000));

  /* The clock resumed from the target, not from the pre-seek position. */
  fake_advance (&clock, 60000);
  g_assert_cmpint (oe_playback_session_get_position (fx->session), ==, G_GINT64_CONSTANT (5060000));

  oe_playback_session_stop (fx->session);
}

/* ------------------------------------------------------------------ */
/* Missing media                                                       */
/* ------------------------------------------------------------------ */

static void
test_missing_media_reports_once_and_transport_continues (ClockFixture *fx,
                                                         gconstpointer user_data G_GNUC_UNUSED)
{
  fx->project = build_project_25fps ();
  const guint ref = add_missing_media (fx->project);

  add_video_clip (fx->project, ref, 0, 0, G_GINT64_CONSTANT (30000000));

  fx->session = oe_playback_session_new ((const OeProject *) fx->project);
  FakeClock clock = { 0 };
  oe_playback_session_set_time_source (fx->session, fake_time, &clock);
  oe_playback_session_set_event_func (fx->session, count_event, fx);

  GError *error = NULL;

  g_assert_true (oe_playback_session_play (fx->session, &error));

  /* Video open failures are synchronous in tick: the first tick reports
   * the missing file once, and later ticks stay quiet. */
  oe_playback_session_tick (fx->session);
  fake_advance (&clock, 20000);
  oe_playback_session_tick (fx->session);

  g_assert_cmpuint (fx->event_count, ==, 1);
  g_assert_cmpint (fx->last_event, ==, OE_PLAYBACK_EVENT_MISSING_MEDIA_SKIPPED);
  g_assert_cmpint (oe_playback_session_get_state (fx->session), ==, OE_PLAYBACK_PLAYING);

  oe_playback_session_stop (fx->session);
}

/* ------------------------------------------------------------------ */

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  /* The session opens an SDL stream at play(): init the audio adapter
   * for the whole run (dummy driver via the meson test environment). */
  GError *audio_error = NULL;

  if (!oe_audio_output_init (&audio_error))
    {
      g_printerr ("audio init failed: %s\n", audio_error->message);
      g_error_free (audio_error);
      return 1;
    }

#define CLOCK_ADD(path, fn)                                                                        \
  g_test_add ("/clock" path, ClockFixture, NULL, clock_fixture_setup, fn, clock_fixture_teardown)

  CLOCK_ADD ("/mapping/topmost-track-wins", test_mapping_topmost_track_wins);
  CLOCK_ADD ("/mapping/source-clamped-into-range", test_mapping_source_clamped_into_range);
  CLOCK_ADD ("/empty/reports-nothing-to-play", test_empty_sequence_reports_nothing_to_play);
  CLOCK_ADD ("/deadline/future-and-frame-spaced", test_tick_deadlines_are_future_and_frame_spaced);
  CLOCK_ADD ("/end-of-sequence/stops-and-parks", test_end_of_sequence_stops_and_parks);
  CLOCK_ADD ("/drift/pause-resume-reanchors", test_pause_resume_reanchors);
  CLOCK_ADD ("/stop/parks-and-play-continues", test_stop_parks_and_play_continues);
  CLOCK_ADD ("/seek/clamps", test_seek_clamps);
  CLOCK_ADD ("/seek/during-playback-resets-clock", test_seek_during_playback_resets_clock);
  CLOCK_ADD ("/missing-media/reports-once-transport-continues",
             test_missing_media_reports_once_and_transport_continues);

#undef CLOCK_ADD

  const int result = g_test_run ();

  oe_audio_output_shutdown ();
  return result;
}
