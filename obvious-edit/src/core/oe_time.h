/* oe_time.h — rational time primitives (Phase 3).
 *
 * The time-model floor from project-format.md, made type-level: integer
 * microseconds everywhere, frame rates as reduced num/den rationals, and
 * no `double` in any signature that could leak into serialized state
 * — the "Time and metadata floors" section of architecture.md.
 *
 * Rounding contract (every conversion in this module): round to nearest,
 * halves away from zero — exactly once, at the final step. Intermediates
 * are exact integer arithmetic, so a frame->us->frame round trip is the
 * identity for every representable frame.
 *
 * GTK-free, GLib-only: this module lives in the core layer and never
 * includes gtk.h, FFmpeg, or SDL headers.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * OeRational: a reduced rational number with a positive denominator.
 * @num: numerator (may be negative)
 * @den: denominator, always > 0
 *
 * The struct invariant is maintained by every constructor in this
 * module: values are always in lowest terms and @den is always
 * positive. A den == 0 value can only exist by casting raw memory —
 * treat that as a programming error, never a valid state.
 */
typedef struct
{
  gint64 num;
  gint64 den;
} OeRational;

/**
 * OE_TIME_ERROR: error domain for time construction failures.
 */
#define OE_TIME_ERROR (oe_time_error_quark ())

GQuark oe_time_error_quark (void);

/**
 * OeTimeError:
 * @OE_TIME_ERROR_BAD_DENOMINATOR: denominator is zero or negative.
 * @OE_TIME_ERROR_NOT_POSITIVE: numerator is zero or negative — a zero
 *   rate (0/0, 0/1, …) has no frame grid and every conversion would
 *   divide by zero; a negative rate is not a rate. 0/0 is the canonical
 *   instance of this family.
 */
typedef enum
{
  OE_TIME_ERROR_BAD_DENOMINATOR,
  OE_TIME_ERROR_NOT_POSITIVE,
} OeTimeError;

/**
 * oe_time_rate:
 * @num: rate numerator
 * @den: rate denominator
 * @error: return location for a #GError, or NULL to ignore
 *
 * Constructs a frame rate from raw integers. Rejects @den <= 0 (which
 * includes 0/0) and non-positive numerators; on success the result is
 * reduced (30000/1001 stays, 50/2 becomes 25/1) with a positive
 * denominator.
 *
 * Returns: the reduced rate, or {0, 0} on failure.
 */
OeRational oe_time_rate (gint64 num, gint64 den, GError **error);

/**
 * oe_time_rate_reduce:
 * @num: numerator (any sign)
 * @den: denominator, nonzero
 *
 * Reduces any integer ratio to lowest terms with a positive
 * denominator, normalizing the sign into the numerator (25/-1 becomes
 * -25/1). This is the primitive that keeps the #OeRational invariant;
 * validation of what constitutes a meaningful rate lives in
 * oe_time_rate().
 *
 * Returns: the reduced ratio, or {0, 0} when @den is zero (0/0 has no
 * reduced form).
 */
OeRational oe_time_rate_reduce (gint64 num, gint64 den);

/**
 * oe_time_round_ratio:
 * @num: numerator (any sign)
 * @den: denominator, > 0
 *
 * The module's rounding primitive: @num / @den rounded to nearest,
 * halves away from zero, in pure integer arithmetic (-5/2 -> -3, 5/2 ->
 * 3, 4/2 -> 2).
 *
 * Returns: the rounded quotient.
 */
gint64 oe_time_round_ratio (gint64 num, gint64 den);

/**
 * oe_time_frame_to_us:
 * @frame: frame count (may be negative)
 * @rate: frame rate from oe_time_rate()
 *
 * Converts a frame position to integer microseconds. The math is exact
 * until the single final rounding; intermediates stay within gint64
 * for any project-scale input (frame counts below ~10^12 at standard
 * rates). A rate with a non-positive numerator is outside the domain —
 * build rates with oe_time_rate().
 *
 * Returns: rounded microseconds.
 */
gint64 oe_time_frame_to_us (gint64 frame, OeRational rate);

/**
 * oe_time_us_to_frame:
 * @us: integer microseconds (may be negative)
 * @rate: frame rate from oe_time_rate()
 *
 * Inverse of oe_time_frame_to_us(): for every frame f in the domain,
 * us_to_frame (frame_to_us (f, rate), rate) == f. Exact until the
 * single final rounding (nearest, halves away from zero — a
 * half-frame offset rounds away from the frame grid origin).
 *
 * Returns: rounded frame count.
 */
gint64 oe_time_us_to_frame (gint64 us, OeRational rate);

G_END_DECLS
