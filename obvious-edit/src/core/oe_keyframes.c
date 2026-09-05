/* oe_keyframes.c — keyframe store and interpolation (Phase 9 Wave B).
 *
 * Every answer here is pure integer arithmetic over the sorted entry
 * array: the interpolation formula calls oe_time_round_ratio exactly
 * once, at the final step, and nothing else rounds. Degradation cases
 * (empty, single-entry, unsorted, zero-span) answer the caller's
 * static value instead of guessing an animation.
 */

#include "oe_keyframes.h"

#include <string.h>

static const struct
{
  OeKeyframeProperty property;
  const gchar *name;
} property_names[] = {
  { OE_KEYFRAME_OPACITY, "opacity" },
  { OE_KEYFRAME_POS_X, "pos-x" },
  { OE_KEYFRAME_POS_Y, "pos-y" },
  { OE_KEYFRAME_SCALE_PERMILLE, "scale-permille" },
  { OE_KEYFRAME_ROTATION_CDEG, "rotation-cdeg" },
};

const gchar *
oe_keyframe_property_get_name (OeKeyframeProperty property)
{
  for (gsize i = 0; i < G_N_ELEMENTS (property_names); i++)
    {
      if (property_names[i].property == property)
        return property_names[i].name;
    }

  g_return_val_if_reached ("opacity");
}

gboolean
oe_keyframe_property_parse (const gchar *name, OeKeyframeProperty *out)
{
  if (name == NULL || out == NULL)
    return FALSE;

  for (gsize i = 0; i < G_N_ELEMENTS (property_names); i++)
    {
      if (g_strcmp0 (property_names[i].name, name) == 0)
        {
          *out = property_names[i].property;
          return TRUE;
        }
    }

  return FALSE;
}

gboolean
oe_keyframe_value_in_domain (OeKeyframeProperty property, gint32 value)
{
  switch (property)
    {
    case OE_KEYFRAME_OPACITY:
      /* guint8 domain: 0–255. */
      return value >= 0 && value <= 255;
    case OE_KEYFRAME_SCALE_PERMILLE:
      return value >= 1 && value <= 32000;
    case OE_KEYFRAME_ROTATION_CDEG:
      return value >= -36000 && value <= 36000;
    case OE_KEYFRAME_POS_X:
    case OE_KEYFRAME_POS_Y:
      /* The full gint32 domain is the documented one for positions —
       * the UI and the compositor bound them for presentation. */
      return TRUE;
    default:
      return FALSE; /* outside the closed property set */
    }
}

gint
oe_keyframe_cmp (gconstpointer a, gconstpointer b, gpointer user_data G_GNUC_UNUSED)
{
  const OeKeyframe *ka = a;
  const OeKeyframe *kb = b;

  if (ka->property != kb->property)
    return ka->property < kb->property ? -1 : 1;

  if (ka->time_us != kb->time_us)
    return ka->time_us < kb->time_us ? -1 : 1;

  return 0;
}

GArray *
oe_keyframes_copy_array (const GArray *src)
{
  if (src == NULL)
    return NULL;

  /* A fresh array, not a ref: value semantics keep every holder
   * (undo records, deep-copied clips, mutator staging) independent,
   * so mutating one owner's array can never alias another's. */
  GArray *copy = g_array_sized_new (FALSE, FALSE, sizeof (OeKeyframe), src->len);

  if (src->len > 0)
    g_array_append_vals (copy, src->data, src->len);

  return copy;
}

gboolean
oe_keyframes_equal (const GArray *a, const GArray *b)
{
  const guint n_a = a != NULL ? a->len : 0;
  const guint n_b = b != NULL ? b->len : 0;

  if (n_a != n_b)
    return FALSE;

  for (guint i = 0; i < n_a; i++)
    {
      /* Field-wise, never memcmp: OeKeyframe carries padding bytes a
       * compound literal never initializes. */
      const OeKeyframe *ka = &g_array_index (a, OeKeyframe, i);
      const OeKeyframe *kb = &g_array_index (b, OeKeyframe, i);

      if (ka->property != kb->property || ka->time_us != kb->time_us || ka->value != kb->value)
        return FALSE;
    }

  return TRUE; /* NULL and empty both mean "no keyframes" */
}

gboolean
oe_keyframes_valid (const GArray *keyframes)
{
  if (keyframes == NULL)
    return TRUE;

  for (guint i = 0; i < keyframes->len; i++)
    {
      const OeKeyframe *kf = &g_array_index (keyframes, OeKeyframe, i);

      if (kf->time_us < 0)
        return FALSE;

      if (!oe_keyframe_value_in_domain (kf->property, kf->value))
        return FALSE;
    }

  return TRUE;
}

guint
oe_keyframes_count_for_property (const GArray *keyframes, OeKeyframeProperty property)
{
  guint count = 0;

  if (keyframes == NULL)
    return 0;

  for (guint i = 0; i < keyframes->len; i++)
    {
      if (g_array_index (keyframes, OeKeyframe, i).property == property)
        count++;
    }

  return count;
}

void
oe_keyframes_insert (GArray *keyframes, OeKeyframe keyframe)
{
  g_return_if_fail (keyframes != NULL);

  /* Binary search for the first entry >= @keyframe, then replace an
   * exact key match or splice before it — the array stays sorted by
   * (property, time) and never carries duplicate keys. */
  guint lo = 0;
  guint hi = keyframes->len;

  while (lo < hi)
    {
      const guint mid = lo + (hi - lo) / 2;
      const OeKeyframe *probe = &g_array_index (keyframes, OeKeyframe, mid);
      const gint order = oe_keyframe_cmp (probe, &keyframe, NULL);

      if (order < 0)
        lo = mid + 1;
      else
        hi = mid;
    }

  if (lo < keyframes->len
      && oe_keyframe_cmp (&g_array_index (keyframes, OeKeyframe, lo), &keyframe, NULL) == 0)
    {
      g_array_index (keyframes, OeKeyframe, lo) = keyframe;
      return;
    }

  g_array_insert_vals (keyframes, lo, &keyframe, 1);
}

gboolean
oe_keyframes_remove (GArray *keyframes, OeKeyframeProperty property, gint64 time_us)
{
  g_return_val_if_fail (keyframes != NULL, FALSE);

  const OeKeyframe needle = { property, time_us, 0 };

  for (guint i = 0; i < keyframes->len; i++)
    {
      if (oe_keyframe_cmp (&g_array_index (keyframes, OeKeyframe, i), &needle, NULL) == 0)
        {
          g_array_remove_index (keyframes, i);
          return TRUE;
        }
    }

  return FALSE;
}

/* Linear ramp between two bracketing entries — the one place the
 * contract's single rounding happens. Callers guarantee 0 < delta <
 * den and in-domain values; the saturated fallback covers the
 * unreachable span regime where |dv| * delta would overflow. */
static gint32
ramp (gint32 va, gint32 vb, gint64 delta, gint64 den)
{
  const gint64 dv = (gint64) vb - (gint64) va;
  const gint64 abs_dv = dv < 0 ? -dv : dv;

  if (delta <= 0)
    return va;

  if (delta >= den)
    return vb;

  /* |dv| <= 72000 for the v1 domains, so the product below fits
   * gint64 for any span under ~4 years. Past that guard the frame
   * grid is irrelevant at the scale of the bracket: land on the
   * nearer endpoint. */
  if (delta > G_MAXINT64 / (2 * abs_dv + 1))
    return delta * 2 >= den ? vb : va;

  return (gint32) (va + oe_time_round_ratio (dv * delta, den));
}

/* The property's entry run inside the array: contiguous by the
 * (property, time) ordering. Returns FALSE when the run cannot
 * animate — absent, single-entry, unsorted, or zero-span. */
static gboolean
find_run (const GArray *keyframes, OeKeyframeProperty property, guint *run_start, guint *run_len)
{
  guint start = keyframes->len;
  guint end = keyframes->len;

  for (guint i = 0; i < keyframes->len; i++)
    {
      const OeKeyframe *kf = &g_array_index (keyframes, OeKeyframe, i);

      if (kf->property != property)
        continue;

      if (start == keyframes->len)
        start = i;

      end = i + 1;
    }

  if (start == keyframes->len)
    return FALSE; /* no entries for this property */

  const guint len = end - start;

  if (len < 2)
    return FALSE; /* single entry: nothing to interpolate between */

  const OeKeyframe *first = &g_array_index (keyframes, OeKeyframe, start);
  const OeKeyframe *last = &g_array_index (keyframes, OeKeyframe, end - 1);

  if (first->time_us >= last->time_us)
    return FALSE; /* zero-span or unsorted run: documented degradation */

  *run_start = start;
  *run_len = len;
  return TRUE;
}

gint32
oe_keyframes_sample (const GArray *keyframes, OeKeyframeProperty property, gint64 time_us,
                     gint32 static_value)
{
  if (keyframes == NULL)
    return static_value;

  guint start = 0;
  guint len = 0;

  if (!find_run (keyframes, property, &start, &len))
    return static_value;

  const OeKeyframe *first = &g_array_index (keyframes, OeKeyframe, start);
  const OeKeyframe *last = &g_array_index (keyframes, OeKeyframe, start + len - 1);

  if (time_us <= first->time_us)
    return first->value; /* clamp to the run's left endpoint */

  if (time_us >= last->time_us)
    return last->value; /* clamp to the run's right endpoint */

  /* Strictly inside: the last entry at or before @time_us and the
   * first entry after it. The right neighbor's time is strictly
   * greater than @time_us, so its span is never zero here. */
  const OeKeyframe *left = first;
  const OeKeyframe *right = last;

  for (guint i = start; i < start + len; i++)
    {
      const OeKeyframe *kf = &g_array_index (keyframes, OeKeyframe, i);

      if (kf->time_us <= time_us)
        left = kf;
      else
        {
          right = kf;
          break;
        }
    }

  return ramp (left->value, right->value, time_us - left->time_us, right->time_us - left->time_us);
}
