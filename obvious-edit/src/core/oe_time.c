/* oe_time.c — rational time primitives (Phase 3).
 *
 * Every function here is pure integer arithmetic: one division, one
 * remainder, one rounding decision. Rounding happens exactly once per
 * conversion, at the final step, nearest with halves away from zero —
 * so conversions compose into identities instead of accumulating drift.
 */

#include "oe_time.h"

G_DEFINE_QUARK (oe - time - error, oe_time_error)

static gint64
gcd64 (gint64 a, gint64 b)
{
  gint64 t;

  a = a < 0 ? -a : a;
  b = b < 0 ? -b : b;

  while (b != 0)
    {
      t = a % b;
      a = b;
      b = t;
    }

  return a;
}

OeRational
oe_time_rate_reduce (gint64 num, gint64 den)
{
  /* The compound literal cannot sit inside g_return_val_if_fail: the
   * comma in { 0, 0 } would split the macro argument. */
  const OeRational invalid = { 0, 0 };

  g_return_val_if_fail (den != 0, invalid);

  if (den < 0)
    {
      num = -num;
      den = -den;
    }

  gint64 g = gcd64 (num, den);

  if (g > 1)
    {
      num /= g;
      den /= g;
    }

  return (OeRational) { num, den };
}

OeRational
oe_time_rate (gint64 num, gint64 den, GError **error)
{
  if (den <= 0)
    {
      g_set_error (error, OE_TIME_ERROR, OE_TIME_ERROR_BAD_DENOMINATOR,
                   "invalid rate %lld/%lld: denominator must be positive", (long long) num,
                   (long long) den);
      return (OeRational) { 0, 0 };
    }

  if (num <= 0)
    {
      g_set_error (error, OE_TIME_ERROR, OE_TIME_ERROR_NOT_POSITIVE,
                   "invalid rate %lld/%lld: a frame rate must be positive (0/0 and "
                   "zero or negative rates have no frame grid)",
                   (long long) num, (long long) den);
      return (OeRational) { 0, 0 };
    }

  return oe_time_rate_reduce (num, den);
}

gint64
oe_time_round_ratio (gint64 num, gint64 den)
{
  gint64 r;

  g_return_val_if_fail (den > 0, 0);

  gint64 q = num / den;

  r = num % den;

  /* |r| >= ceil (den / 2) is exactly "the fractional part is >= 0.5"
   * in integer form (for even den the equality case included), and the
   * rounding direction follows the sign of the remainder — away from
   * zero. */
  gint64 magnitude = r < 0 ? -r : r;

  if (magnitude >= (den + 1) / 2)
    q += (r < 0) ? -1 : 1;

  return q;
}

gint64
oe_time_frame_to_us (gint64 frame, OeRational rate)
{
  g_return_val_if_fail (rate.num > 0 && rate.den > 0, 0);

  /* us = frame * rate.den * 10^6 / rate.num, rounded once. Split the
   * division first (q + remainder) so the 10^6 scale multiplies a
   * quotient that already fits the microseconds domain: the
   * remainder-scaled term is bounded by rate.num * 10^6. */
  gint64 n = frame * rate.den;
  gint64 q = n / rate.num;
  gint64 r = n % rate.num;

  return q * G_GINT64_CONSTANT (1000000)
         + oe_time_round_ratio (r * G_GINT64_CONSTANT (1000000), rate.num);
}

gint64
oe_time_us_to_frame (gint64 us, OeRational rate)
{
  g_return_val_if_fail (rate.num > 0 && rate.den > 0, 0);

  /* frame = us * rate.num / (rate.den * 10^6), rounded once — the same
   * quotient-plus-remainder split, in the inverse direction. */
  gint64 d = rate.den * G_GINT64_CONSTANT (1000000);
  gint64 q = us / d;
  gint64 r = us % d;

  return q * rate.num + oe_time_round_ratio (r * rate.num, d);
}
