/* test_time.c — GTK-free tests for rational time (Phase 3).
 *
 *   /time/rate-reduces               rates are always reduced, den > 0.
 *   /time/rate-rejects-bad-dens      den <= 0 and 0/0 fail with an error.
 *   /time/rate-rejects-non-positive  num <= 0 with den > 0 fails too.
 *   /time/ntsc-frame-conversions     30000/1001 frame<->us, exact math.
 *   /time/frame-identities           us->frame->us identity at 24, 25,
 *                                    30000/1001, and 48000/1 "fps".
 *   /time/rounding-halves-away       exact .5 boundaries round away from
 *                                    zero, in both directions.
 *   /time/round-ratio                oe_time_round_ratio corner cases.
 */

#include <glib.h>

#include "../src/core/oe_time.h"

/* --- rate construction ------------------------------------------------- */

static void
test_rate_reduces (void)
{
  GError *error = NULL;

  OeRational r = oe_time_rate (50, 25, &error);

  g_assert_no_error (error);
  g_assert_cmpint (r.num, ==, 2);
  g_assert_cmpint (r.den, ==, 1);

  /* NTSC and film rates are already coprime: they pass through. */
  OeRational ntsc = oe_time_rate (30000, 1001, &error);

  g_assert_no_error (error);
  g_assert_cmpint (ntsc.num, ==, 30000);
  g_assert_cmpint (ntsc.den, ==, 1001);

  OeRational f23976 = oe_time_rate (24000, 1001, &error);

  g_assert_no_error (error);
  g_assert_cmpint (f23976.num, ==, 24000);
  g_assert_cmpint (f23976.den, ==, 1001);

  /* oe_time_rate_reduce never fails: it reduces and normalizes. */
  OeRational reduced = oe_time_rate_reduce (-6, -4);

  g_assert_cmpint (reduced.num, ==, 3);
  g_assert_cmpint (reduced.den, ==, 2);

  reduced = oe_time_rate_reduce (0, 7);

  g_assert_cmpint (reduced.num, ==, 0);
  g_assert_cmpint (reduced.den, ==, 1);

  /* Sign lives in the numerator; the denominator stays positive. */
  reduced = oe_time_rate_reduce (25, -1);

  g_assert_cmpint (reduced.num, ==, -25);
  g_assert_cmpint (reduced.den, ==, 1);

  /* 0/0 has no reduced form: the {0, 0} sentinel comes back, with a
   * programming-error critical on the path (expected here). */
  g_test_expect_message ("oe", G_LOG_LEVEL_CRITICAL, "*den != 0*");
  reduced = oe_time_rate_reduce (0, 0);
  g_test_assert_expected_messages ();
  g_assert_cmpint (reduced.num, ==, 0);
  g_assert_cmpint (reduced.den, ==, 0);
}

static void
test_rate_rejects_bad_dens (void)
{
  GError *error = NULL;

  /* Zero and negative denominators are rejected, including the 0/0
   * degenerate; the sentinel return is the {0, 0} zero value. */
  OeRational r = oe_time_rate (25, 0, &error);

  g_assert_error (error, OE_TIME_ERROR, OE_TIME_ERROR_BAD_DENOMINATOR);
  g_assert_cmpint (r.num, ==, 0);
  g_assert_cmpint (r.den, ==, 0);
  g_clear_error (&error);

  r = oe_time_rate (25, -1, &error);

  g_assert_error (error, OE_TIME_ERROR, OE_TIME_ERROR_BAD_DENOMINATOR);
  g_clear_error (&error);

  r = oe_time_rate (0, 0, &error);

  g_assert_error (error, OE_TIME_ERROR, OE_TIME_ERROR_BAD_DENOMINATOR);
  g_clear_error (&error);
}

static void
test_rate_rejects_non_positive (void)
{
  GError *error = NULL;

  /* With a valid denominator, a non-positive numerator has no frame
   * grid: 0/1 and -25/1 are not rates. */
  OeRational r = oe_time_rate (0, 1, &error);

  g_assert_error (error, OE_TIME_ERROR, OE_TIME_ERROR_NOT_POSITIVE);
  g_assert_cmpint (r.num, ==, 0);
  g_assert_cmpint (r.den, ==, 0);
  g_clear_error (&error);

  r = oe_time_rate (-25, 1, &error);

  g_assert_error (error, OE_TIME_ERROR, OE_TIME_ERROR_NOT_POSITIVE);
  g_assert_cmpint (r.num, ==, 0);
  g_assert_cmpint (r.den, ==, 0);
  g_clear_error (&error);
}

/* --- frame<->microsecond conversions ----------------------------------- */

/* oe_time_frame_to_us: frame * 1_000_000 / num * den, exact. */
static void
test_ntsc_frame_conversions (void)
{
  GError *error = NULL;
  OeRational ntsc = oe_time_rate (30000, 1001, &error);

  g_assert_no_error (error);

  /* Frame 0 is the origin. */
  g_assert_cmpint (oe_time_frame_to_us (0, ntsc), ==, 0);

  /* One NTSC frame = 1001/30 ms = 33366.67 us; the conversion is the
   * exact integer 1_000_000 * 1001 / 30000. */
  g_assert_cmpint (oe_time_frame_to_us (1, ntsc), ==, 33367);
  g_assert_cmpint (oe_time_frame_to_us (30, ntsc), ==, 1001000); /* 1001 ms */
  g_assert_cmpint (oe_time_frame_to_us (30000, ntsc), ==, 1001000000);

  /* Back to frames: every exact frame boundary is recovered exactly. */
  g_assert_cmpint (oe_time_us_to_frame (33367, ntsc), ==, 1);
  g_assert_cmpint (oe_time_us_to_frame (1001000, ntsc), ==, 30);
  g_assert_cmpint (oe_time_us_to_frame (1001000000, ntsc), ==, 30000);

  /* Just under a boundary rounds down; just over the halfway point
   * between frames 0 and 1 (16683.5 us) rounds up — nearest. */
  g_assert_cmpint (oe_time_us_to_frame (33366, ntsc), ==, 1);
  g_assert_cmpint (oe_time_us_to_frame (16684, ntsc), ==, 1);
  g_assert_cmpint (oe_time_us_to_frame (16683, ntsc), ==, 0);
}

static void
test_frame_identities (void)
{
  struct
  {
    gint64 num, den;
  } rates[] = {
    { 24, 1 },       /* film */
    { 25, 1 },       /* PAL */
    { 30000, 1001 }, /* NTSC */
    { 48000, 1 },    /* audio-rate "frames" (samples) */
  };

  for (gsize i = 0; i < G_N_ELEMENTS (rates); i++)
    {
      GError *error = NULL;
      OeRational rate = oe_time_rate (rates[i].num, rates[i].den, &error);

      g_assert_no_error (error);

      /* us -> frame -> us is an identity on exact boundaries. */
      for (gint64 f = 0; f <= 96; f++)
        {
          gint64 us = oe_time_frame_to_us (f, rate);

          g_assert_cmpint (oe_time_us_to_frame (us, rate), ==, f);
        }

      /* Monotonic: later frames never land earlier. */
      gint64 prev = oe_time_frame_to_us (0, rate);

      for (gint64 f = 1; f <= 1000; f++)
        {
          gint64 us = oe_time_frame_to_us (f, rate);

          g_assert_cmpint (us, >, prev);
          prev = us;
        }
    }
}

/* --- rounding: nearest, halves away from zero --------------------------- */

static void
test_rounding_halves_away (void)
{
  GError *error = NULL;

  /* At 4 fps one frame is exactly 250000 us, so 125000 us is the exact
   * midpoint between frames 0 and 1 -> rounds away from zero. */
  OeRational four = oe_time_rate (4, 1, &error);

  g_assert_no_error (error);
  g_assert_cmpint (oe_time_us_to_frame (125000, four), ==, 1);

  /* Negative time uses the same rule mirrored: -0.5 frames -> -1. */
  g_assert_cmpint (oe_time_us_to_frame (-125000, four), ==, -1);

  /* Strictly inside the interval rounds toward zero here. */
  g_assert_cmpint (oe_time_us_to_frame (124999, four), ==, 0);
  g_assert_cmpint (oe_time_us_to_frame (-124999, four), ==, 0);
}

static void
test_round_ratio (void)
{
  /* Exact division needs no rounding. */
  g_assert_cmpint (oe_time_round_ratio (10, 2), ==, 5);
  g_assert_cmpint (oe_time_round_ratio (-10, 2), ==, -5);

  /* Halves away from zero. */
  g_assert_cmpint (oe_time_round_ratio (5, 2), ==, 3);
  g_assert_cmpint (oe_time_round_ratio (-5, 2), ==, -3);
  g_assert_cmpint (oe_time_round_ratio (1, 2), ==, 1);
  g_assert_cmpint (oe_time_round_ratio (-1, 2), ==, -1);

  /* Plain nearest below the midpoint. */
  g_assert_cmpint (oe_time_round_ratio (4, 3), ==, 1);
  g_assert_cmpint (oe_time_round_ratio (-4, 3), ==, -1);
  g_assert_cmpint (oe_time_round_ratio (0, 7), ==, 0);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/time/rate-reduces", test_rate_reduces);
  g_test_add_func ("/time/rate-rejects-bad-dens", test_rate_rejects_bad_dens);
  g_test_add_func ("/time/rate-rejects-non-positive", test_rate_rejects_non_positive);
  g_test_add_func ("/time/ntsc-frame-conversions", test_ntsc_frame_conversions);
  g_test_add_func ("/time/frame-identities", test_frame_identities);
  g_test_add_func ("/time/rounding-halves-away", test_rounding_halves_away);
  g_test_add_func ("/time/round-ratio", test_round_ratio);

  return g_test_run ();
}
