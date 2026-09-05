/* test_audio_tools.c — the audio-tools suite (Phase 10 Wave B, 18th).
 *
 * Pure buffer and meter math, GTK-free and fixture-free:
 *   1. factor-buffer — per-channel peak extraction over the shared
 *      interleaved f32 format, and the Wave A factor chain applied to
 *      a buffer (buffer-constant gains, per-channel peaks, equal-sum
 *      pan, the final clamp);
 *   2. meter-decay — the per-update peak-hold-and-fall rule;
 *   3. mixer-geometry — the meter's Cairo bar layout.
 *
 * The parity proof for the multi-track mixer lives in the export
 * suite (test_parity_audio_mix); this suite pins the math it relies
 * on. No GTK, no media, no display: the widget is a shell over these
 * helpers.
 */

#include "src/core/oe_audio_buffer.h"
#include "src/core/oe_audio_factor.h"
#include "src/ui/oe_meter_math.h"

#include <glib.h>

/* ------------------------------------------------------------------ */
/* 1. factor-buffer: peak extraction and factor application           */
/* ------------------------------------------------------------------ */

static void
test_peak_per_channel (void)
{
  const gfloat stereo[] = { 0.5f, -0.25f, -0.75f, 0.125f };

  g_assert_cmpfloat (oe_audio_buffer_peak (stereo, 2, 2, 0), ==, 0.75f);
  g_assert_cmpfloat (oe_audio_buffer_peak (stereo, 2, 2, 1), ==, 0.25f);
}

static void
test_peak_edge_cases (void)
{
  /* NULL and empty spans read as silence; out-of-range channels clamp
   * to the first channel rather than read out of bounds. */
  g_assert_cmpfloat (oe_audio_buffer_peak (NULL, 4, 2, 0), ==, 0.0f);

  const gfloat mono[] = { 0.25f, -0.5f };

  g_assert_cmpfloat (oe_audio_buffer_peak (mono, 0, 1, 0), ==, 0.0f);
  g_assert_cmpfloat (oe_audio_buffer_peak (mono, 1, 1, 7), ==, 0.25f);
  g_assert_cmpfloat (oe_audio_buffer_peak (mono, 2, 1, 0), ==, 0.5f);
}

static void
test_peak_multi_frame (void)
{
  /* The peak is over the whole span, not per frame: ch1's max sits in
   * the second frame. */
  const gfloat span[] = {
    0.1f, 0.05f, -0.2f, 0.3f, 0.15f, -0.45f,
  };

  g_assert_cmpfloat (oe_audio_buffer_peak (span, 3, 2, 0), ==, 0.2f);
  g_assert_cmpfloat (oe_audio_buffer_peak (span, 3, 2, 1), ==, 0.45f);
}

static void
test_factor_scales_buffer_peaks (void)
{
  /* Half track volume halves every channel's peak; the factor is
   * buffer-constant, so the waveform shape only scales. */
  const gfloat input[] = { 0.5f, -0.25f, -0.75f, 0.125f };
  gfloat buffer[] = { 0.5f, -0.25f, -0.75f, 0.125f };
  gint32 factor[2];

  oe_audio_factor (1024, 1024, 512, 512, 512, 1, factor);
  g_assert_cmpint (factor[0], ==, 512); /* half volume, centered */
  g_assert_cmpint (factor[1], ==, 512);

  for (gsize i = 0; i < G_N_ELEMENTS (buffer); i++)
    buffer[i] = buffer[i] * (gfloat) factor[i % 2] / (gfloat) OE_AUDIO_UNITY;

  g_assert_cmpfloat (oe_audio_buffer_peak (buffer, 2, 2, 0), ==, 0.375f);
  g_assert_cmpfloat (oe_audio_buffer_peak (buffer, 2, 2, 1), ==, 0.125f);
  g_assert_cmpfloat (oe_audio_buffer_peak (input, 2, 2, 0), ==, 0.75f);
}

static void
test_factor_hard_pan_routes_peak (void)
{
  /* Hard right: every left sample zeroes, the right channel carries
   * the doubled (+6 dB) chain — the equal-sum law's routing. */
  gfloat buffer[] = { 0.25f, 0.25f, 0.25f, 0.25f };
  gint32 factor[2];

  oe_audio_factor (1024, 1024, 512, 1024, 1024, 1, factor);
  g_assert_cmpint (factor[0], ==, 0);
  g_assert_cmpint (factor[1], ==, 2048);

  for (gsize i = 0; i < G_N_ELEMENTS (buffer); i++)
    buffer[i] = buffer[i] * (gfloat) factor[i % 2] / (gfloat) OE_AUDIO_UNITY;

  g_assert_cmpfloat (oe_audio_buffer_peak (buffer, 2, 2, 0), ==, 0.0f);
  g_assert_cmpfloat (oe_audio_buffer_peak (buffer, 2, 2, 1), ==, 0.5f);
}

static void
test_factor_clamps_buffer_peak (void)
{
  /* The mixdown's final clamp keeps the mixed sample in [−1, 1]: two
   * near-unity contributions (0.9 + 0.9) sum above 1 on both
   * interleaved channels and must not overflow the meter domain. */
  gfloat buffer[] = { 0.9f, 0.9f, 0.9f, 0.9f };

  for (gsize i = 0; i < G_N_ELEMENTS (buffer); i++)
    buffer[i] = CLAMP (buffer[i] + 0.9f, -1.0f, 1.0f);

  g_assert_cmpfloat (oe_audio_buffer_peak (buffer, 2, 2, 0), ==, 1.0f);
  g_assert_cmpfloat (oe_audio_buffer_peak (buffer, 2, 2, 1), ==, 1.0f);
}

/* ------------------------------------------------------------------ */
/* 2. meter-decay: the per-update hold-and-fall rule                  */
/* ------------------------------------------------------------------ */

static void
test_decay_step_documented (void)
{
  g_assert_cmpfloat (oe_meter_decay_step (), ==, 0.04f);
}

static void
test_decay_rises_instantly (void)
{
  /* A peak jumps the bar straight up: no rise ramp. */
  g_assert_cmpfloat (oe_meter_decay_level (0.0f, 0.9f), ==, 0.9f);
  g_assert_cmpfloat (oe_meter_decay_level (0.2f, 0.9f), ==, 0.9f);
}

static void
test_decay_falls_per_update (void)
{
  /* Between chunks the held peak falls one documented step, and
   * falling never dips below the incoming peak. (Epsilon compare:
   * g_assert_cmpfloat is exact, and 0.9f − 0.04f rounds one ULP off
   * the 0.86 literal.) */
  g_assert_cmpfloat_with_epsilon (oe_meter_decay_level (0.9f, 0.5f), 0.86f, 1e-5f);
  g_assert_cmpfloat_with_epsilon (oe_meter_decay_level (0.05f, 0.0f), 0.01f, 1e-5f);
}

static void
test_decay_clamps_domain (void)
{
  /* The rule never reports outside [0, 1], whatever it is fed. */
  g_assert_cmpfloat (oe_meter_decay_level (1.2f, 2.0f), ==, 1.0f);
  g_assert_cmpfloat (oe_meter_decay_level (0.02f, -0.5f), ==, 0.0f);
  g_assert_cmpfloat (oe_meter_decay_level (0.0f, 0.0f), ==, 0.0f);
}

static void
test_decay_sequence_settles (void)
{
  /* A spike then silence: the bar holds near the peak for a few
   * updates and lands exactly at zero — the pause settle the widget's
   * release() short-circuits. */
  gfloat level = 0.0f;

  level = oe_meter_decay_level (level, 0.9f);
  g_assert_cmpfloat (level, ==, 0.9f);

  for (guint i = 0; i < 23; i++) /* 0.9 / 0.04 ≈ 22.5 updates to floor */
    level = oe_meter_decay_level (level, 0.0f);

  g_assert_cmpfloat (level, ==, 0.0f);
}

/* ------------------------------------------------------------------ */
/* 3. mixer-geometry: the meter's bar layout                          */
/* ------------------------------------------------------------------ */

static void
test_geometry_two_bars (void)
{
  int x0 = -1, x1 = -1;

  const int bar = oe_meter_bar_geometry (100, 40, 2, 0, &x0);

  g_assert_cmpint (bar, ==, 48); /* (100 − 4 gap) / 2 */
  g_assert_cmpint (oe_meter_bar_geometry (100, 40, 2, 1, &x1), ==, 48);
  g_assert_cmpint (x0, ==, 0);
  g_assert_cmpint (x1, ==, 52);
}

static void
test_geometry_odd_width (void)
{
  /* Integer math pins the rounding: 101 px cannot split evenly, the
   * spare pixel stays on the right of the centered span. */
  int x0 = -1, x1 = -1;

  g_assert_cmpint (oe_meter_bar_geometry (101, 40, 2, 0, &x0), ==, 48);
  g_assert_cmpint (oe_meter_bar_geometry (101, 40, 2, 1, &x1), ==, 48);
  g_assert_cmpint (x0, ==, 0);
  g_assert_cmpint (x1, ==, 52);
}

static void
test_geometry_minimum_refuses_slivers (void)
{
  /* Below 2 × 8 + 4 = 20 px the layout returns 0 (draw nothing)
   * rather than unreadable bars, and writes no origin. */
  int x = -1;

  g_assert_cmpint (oe_meter_bar_geometry (19, 40, 2, 0, &x), ==, 0);
  g_assert_cmpint (x, ==, -1); /* untouched */
  g_assert_cmpint (oe_meter_bar_geometry (20, 40, 2, 0, &x), ==, 8);
  g_assert_cmpint (x, ==, 0);
}

static void
test_geometry_degenerate_inputs (void)
{
  int x = 7;

  g_assert_cmpint (oe_meter_bar_geometry (100, 40, 0, 0, &x), ==, 0);
  g_assert_cmpint (x, ==, 7); /* no bars: no origin written */

  g_assert_cmpint (oe_meter_bar_geometry (100, 40, 2, 5, &x), ==, 48);
  g_assert_cmpint (x, ==, 7); /* out-of-range index: no origin written */

  /* NULL out-pointer is legal — the widget's width probe path. */
  g_assert_cmpint (oe_meter_bar_geometry (100, 40, 2, 0, NULL), ==, 48);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/audio-tools/peak/per-channel", test_peak_per_channel);
  g_test_add_func ("/audio-tools/peak/edge-cases", test_peak_edge_cases);
  g_test_add_func ("/audio-tools/peak/multi-frame", test_peak_multi_frame);
  g_test_add_func ("/audio-tools/factor/scales-buffer-peaks", test_factor_scales_buffer_peaks);
  g_test_add_func ("/audio-tools/factor/hard-pan-routes-peak", test_factor_hard_pan_routes_peak);
  g_test_add_func ("/audio-tools/factor/clamps-buffer-peak", test_factor_clamps_buffer_peak);
  g_test_add_func ("/audio-tools/decay/step-documented", test_decay_step_documented);
  g_test_add_func ("/audio-tools/decay/rises-instantly", test_decay_rises_instantly);
  g_test_add_func ("/audio-tools/decay/falls-per-update", test_decay_falls_per_update);
  g_test_add_func ("/audio-tools/decay/clamps-domain", test_decay_clamps_domain);
  g_test_add_func ("/audio-tools/decay/sequence-settles", test_decay_sequence_settles);
  g_test_add_func ("/audio-tools/geometry/two-bars", test_geometry_two_bars);
  g_test_add_func ("/audio-tools/geometry/odd-width", test_geometry_odd_width);
  g_test_add_func ("/audio-tools/geometry/minimum-refuses-slivers",
                   test_geometry_minimum_refuses_slivers);
  g_test_add_func ("/audio-tools/geometry/degenerate-inputs", test_geometry_degenerate_inputs);

  return g_test_run ();
}
