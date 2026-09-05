/* oe_keyframes.h — GTK-free keyframe store and interpolation (Phase 9 Wave B).
 *
 * The keyed-property floor from the Phase 9 spec (D7): a clip visual
 * property carries a sorted array of {time_us, value} entries; sampling
 * at a time between entries linearly interpolates with EXACTLY ONE
 * rounding — the final step through oe_time_round_ratio. Everything in
 * this module is pure integer arithmetic over plain GLib arrays: no
 * GTK, no clocks, no side effects, and no `double` anywhere.
 *
 * Representation (locked decision D7):
 *
 *   - entries are #OeKeyframe values in one GArray owned by the
 *     clip's #OeClipVisual, kept sorted by (property, time) — each
 *     property's entries therefore form a contiguous sorted run;
 *   - times are clip-relative RAW microseconds (0 = the clip's first
 *     frame); the model stores raw times and the UI snaps to the frame
 *     grid on input;
 *   - the keyframeable set v1 is opacity plus the transform quad
 *     (pos_x, pos_y, scale_permille, rotation_cdeg); crop stays
 *     static — it has no enum value, so a document naming it is
 *     rejected by the closed property set;
 *   - degradation is the documented safety net, not a silent repair:
 *     an empty, single-entry, unsorted, or zero-span run answers the
 *     clip's static value at sampling time. The validated mutators
 *     never produce such arrays, but a loaded document may carry one —
 *     it loads, it samples as static, and it round-trips unchanged.
 *
 * Interpolation contract (the one the unit tests pin):
 *
 *   value = va + oe_time_round_ratio ((gint64)(vb - va) * (t - ta),
 *                                      tb - ta)
 *
 * with va/vb the bracketing entries and ta/tb their times — one
 * division, one rounding, at the final step only. Times outside the
 * run clamp to the first or last entry's value.
 */

#pragma once

#include <glib.h>

#include "oe_time.h"

G_BEGIN_DECLS

/**
 * OeKeyframeProperty: the keyframeable visual properties (v1).
 *
 * Closed on purpose: a document or mutator call naming anything else
 * is a domain error. Crop is intentionally absent — it stays static in
 * v1 per the spec.
 */
typedef enum
{
  OE_KEYFRAME_OPACITY,
  OE_KEYFRAME_POS_X,
  OE_KEYFRAME_POS_Y,
  OE_KEYFRAME_SCALE_PERMILLE,
  OE_KEYFRAME_ROTATION_CDEG,
} OeKeyframeProperty;

/**
 * OeKeyframe: one keyed sample. @time_us is clip-relative, >= 0;
 * @value lives in the property's documented domain (the same ranges
 * the static visual fields validate against).
 */
typedef struct
{
  OeKeyframeProperty property;
  gint64 time_us;
  gint32 value;
} OeKeyframe;

/**
 * oe_keyframe_property_get_name:
 * @property: a keyframeable property
 *
 * The stable serialization name ("opacity", "pos-x", …) — the same
 * token the JSON reader parses back with
 * oe_keyframe_property_parse().
 *
 * Returns: the property's name, never NULL.
 */
const gchar *oe_keyframe_property_get_name (OeKeyframeProperty property);

/**
 * oe_keyframe_property_parse:
 * @name: serialization name
 * @out: receives the property
 *
 * Strict inverse of oe_keyframe_property_get_name(): unknown names
 * answer FALSE untouched — the closed property set is the reader's
 * validation.
 *
 * Returns: TRUE when @name is a known property.
 */
gboolean oe_keyframe_property_parse (const gchar *name, OeKeyframeProperty *out);

/**
 * oe_keyframe_value_in_domain:
 * @property: a keyframeable property
 * @value: candidate value
 *
 * TRUE when @value fits the property's documented range — the same
 * bounds the static visual fields validate (scale 1–32000 permille,
 * rotation ±36000 cdeg, opacity 0–255, positions the full gint32).
 */
gboolean oe_keyframe_value_in_domain (OeKeyframeProperty property, gint32 value);

/**
 * oe_keyframe_cmp:
 * @a: #OeKeyframe pointers
 * @b: #OeKeyframe pointers
 * @user_data: unused
 *
 * GCompareFunc ordering by (property, time): per-property runs stay
 * contiguous and time-sorted. Two entries sharing property AND time
 * are the same key — g_array binary insertion replaces them.
 */
gint oe_keyframe_cmp (gconstpointer a, gconstpointer b, gpointer user_data);

/**
 * oe_keyframes_copy_array:
 * @src: keyframe array (or NULL)
 *
 * Value-semantics copy for #OeClipVisual ownership: NULL passes
 * through as NULL (no keyframes), otherwise a fresh GArray with the
 * same entries.
 *
 * Returns: (transfer full): the copy, or NULL.
 */
GArray *oe_keyframes_copy_array (const GArray *src);

/**
 * oe_keyframes_equal:
 * @a: keyframe array (or NULL)
 * @b: keyframe array (or NULL)
 *
 * Content equality for undo-record delta detection. NULL and an empty
 * array are equivalent — both mean "no keyframes".
 */
gboolean oe_keyframes_equal (const GArray *a, const GArray *b);

/**
 * oe_keyframes_valid:
 * @keyframes: keyframe array (or NULL)
 *
 * Per-entry domain check: every property is in the closed set, every
 * time is >= 0, every value in domain. Sortedness is deliberately NOT
 * required — an unsorted document loads and samples as static (see
 * oe_keyframes_sample); the validated mutators are the only producers
 * of sorted arrays.
 */
gboolean oe_keyframes_valid (const GArray *keyframes);

/**
 * oe_keyframes_count_for_property:
 * @keyframes: keyframe array (or NULL)
 * @property: which property's run to count
 *
 * Returns: the number of entries in @property's run.
 */
guint oe_keyframes_count_for_property (const GArray *keyframes, OeKeyframeProperty property);

/**
 * oe_keyframes_insert:
 * @keyframes: keyframe array, sorted by oe_keyframe_cmp
 * @keyframe: entry to insert or replace
 *
 * Sorted insertion with replace-on-same-key: an entry sharing
 * (property, time) with an existing one replaces it, so "add a
 * keyframe at t" is idempotent and the array never carries duplicate
 * keys. The mutators' validation step keeps the entry's domain
 * checked before this runs.
 */
void oe_keyframes_insert (GArray *keyframes, OeKeyframe keyframe);

/**
 * oe_keyframes_remove:
 * @keyframes: keyframe array, sorted by oe_keyframe_cmp
 * @property: which property's key to drop
 * @time_us: the exact stored time of the key
 *
 * Returns: TRUE when an entry was removed; FALSE when no key exists
 * at (@property, @time_us) — callers treat that as a no-op.
 */
gboolean oe_keyframes_remove (GArray *keyframes, OeKeyframeProperty property, gint64 time_us);

/**
 * oe_keyframes_sample:
 * @keyframes: keyframe array (or NULL)
 * @property: which property's value to answer
 * @time_us: clip-relative sample time
 * @static_value: the clip's static field value for @property
 *
 * THE interpolation contract (D7). Answers the property's animated
 * value at @time_us:
 *
 *   - no run for @property (or a run shorter than 2 entries, unsorted,
 *     or zero-span — the documented degradation cases) →
 *     @static_value;
 *   - @time_us at or before the run's first entry → that entry's
 *     value; at or after the last entry → that entry's value
 *     (clamp-to-endpoint);
 *   - strictly inside → linear interpolation through
 *     oe_time_round_ratio with exactly one rounding at the final
 *     step.
 *
 * Total over every input: never divides by zero, never reads out of
 * bounds, never returns outside the property's domain for in-domain
 * bracketing entries (interpolation stays between va and vb).
 *
 * Returns: the sampled value.
 */
gint32 oe_keyframes_sample (const GArray *keyframes, OeKeyframeProperty property, gint64 time_us,
                            gint32 static_value);

G_END_DECLS
