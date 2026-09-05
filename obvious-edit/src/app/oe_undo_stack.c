/* oe_undo_stack.c — command-object history (Phase 6). See the header
 * for the sole-path strict-LIFO invariant and the snapshot escape
 * hatch; this file is the mechanics: a bounded record array, a cursor,
 * and inverse replay through the project's validated mutators.
 *
 * Ownership: every record is heap-owned by the stack (label and clip
 * copy). The mutator objects (project, session) are borrowed for the
 * duration of one call — the stack keeps no reference to either.
 */

#include "oe_undo_stack.h"
#include "../core/oe_keyframes.h"

#include "../app/oe_log.h"
#include "oe_playback_session.h"

struct _OeUndoStack
{
  GPtrArray *records; /* owned OeUndoRecord*, ordered oldest → newest */
  guint cursor;       /* next undo reads records[cursor-1], next redo records[cursor] */
  OeUndoChangedFunc changed_func;
  gpointer changed_data;
};

static void
fire_changed (OeUndoStack *self)
{
  if (self->changed_func != NULL)
    self->changed_func (self->cursor > 0, self->cursor < self->records->len, self->changed_data);
}

/* Takes ownership of @label (callers pass freshly formatted strings). */
static OeUndoRecord *
record_new (OeUndoOpKind kind, gchar *label, guint track_index, guint clip_index)
{
  OeUndoRecord *rec = g_new0 (OeUndoRecord, 1);

  rec->kind = kind;
  rec->label = label;
  rec->track_index = track_index;
  rec->clip_index = clip_index;
  rec->ripple_shifts = NULL;
  return rec;
}

static void
record_free (gpointer data)
{
  OeUndoRecord *rec = data;

  g_free (rec->label);
  g_clear_pointer (&rec->ripple_shifts, g_array_unref);
  g_clear_pointer (&rec->transition_reanchors, g_array_unref);
  /* Visuals own keyframe stores since Wave B: both captured visuals
   * release theirs exactly once. */
  g_clear_pointer (&rec->clip.visual.keyframes, g_array_unref);
  g_clear_pointer (&rec->new_visual.keyframes, g_array_unref);
  /* Generators own their text: both recorded generations release
   * theirs exactly once (the keyframe-store precedent). */
  g_clear_pointer (&rec->clip.generator.text, g_free);
  g_clear_pointer (&rec->new_generator.text, g_free);
  g_free (rec);
}

/* Value capture for OeClip/OeClipVisual into record storage: struct
 * copies share the source's keyframe pointer, so the store is replaced
 * with a private deep copy (the pointer overwrite drops the alias
 * without unref-ing the owner's array). */
static void
clip_value_store (OeClip *dst, const OeClip *src)
{
  *dst = *src;
  dst->visual.keyframes = oe_keyframes_copy_array (src->visual.keyframes);
  dst->generator.text = g_strdup (src->generator.text);
}

static void
visual_value_store (OeClipVisual *dst, const OeClipVisual *src)
{
  *dst = *src;
  dst->keyframes = oe_keyframes_copy_array (src->keyframes);
}

/* Self-capture for a value copied out of the live model: the struct
 * copy aliases the clip's keyframe store, which a following mutator
 * may free — replace the alias with a private deep copy. After this
 * call @clip owns its store; assigning it to a record moves that
 * ownership. Callers that do NOT move @clip into a record must
 * release it with clip_capture_clear — every editor path either
 * records or clears, so a rejected or zero-delta edit leaks nothing. */
static void
clip_capture (OeClip *clip)
{
  clip->visual.keyframes = oe_keyframes_copy_array (clip->visual.keyframes);
  clip->generator.text = g_strdup (clip->generator.text);
}

/* Release a captured value that never became a record baseline. */
static void
clip_capture_clear (OeClip *clip)
{
  g_clear_pointer (&clip->visual.keyframes, g_array_unref);
  g_clear_pointer (&clip->generator.text, g_free);
}

/* Phase 11 Wave A stroke recorders, defined below their public entry
 * points (which pair with the audio/visual recorders). */
static gboolean record_generator_stroke (OeProject *project, OeUndoStack *stack, guint track_index,
                                         guint clip_index, const OeClipGenerator *old_generator,
                                         const OeClipGenerator *new_generator, GError **error);
static gboolean record_key_stroke (OeProject *project, OeUndoStack *stack, guint track_index,
                                   guint clip_index, const OeClipKey *old_key,
                                   const OeClipKey *new_key, GError **error);

OeUndoStack *
oe_undo_stack_new (void)
{
  OeUndoStack *self = g_new0 (OeUndoStack, 1);

  self->records = g_ptr_array_new_with_free_func (record_free);
  self->cursor = 0;
  return self;
}

void
oe_undo_stack_free (OeUndoStack *stack)
{
  if (stack == NULL)
    return;

  g_ptr_array_unref (stack->records);
  g_free (stack);
}

void
oe_undo_stack_set_changed_func (OeUndoStack *stack, OeUndoChangedFunc func, gpointer user_data)
{
  g_return_if_fail (stack != NULL);

  stack->changed_func = func;
  stack->changed_data = user_data;
}

void
oe_undo_stack_clear (OeUndoStack *stack)
{
  g_return_if_fail (stack != NULL);

  if (stack->records->len == 0 && stack->cursor == 0)
    return;

  g_ptr_array_remove_range (stack->records, 0, stack->records->len);
  stack->cursor = 0;
  fire_changed (stack);
}

gboolean
oe_undo_stack_can_undo (const OeUndoStack *stack)
{
  return stack != NULL && stack->cursor > 0;
}

gboolean
oe_undo_stack_can_redo (const OeUndoStack *stack)
{
  return stack != NULL && stack->cursor < stack->records->len;
}

guint
oe_undo_stack_get_size (const OeUndoStack *stack)
{
  return stack != NULL ? stack->records->len : 0;
}

/* Appends a record, dropping the redo branch first (any recorded edit
 * discards it — the linear history model) and the oldest record when
 * the depth cap is exceeded. */
static void
stack_push (OeUndoStack *self, OeUndoRecord *rec)
{
  if (self->cursor < self->records->len)
    g_ptr_array_remove_range (self->records, self->cursor, self->records->len - self->cursor);

  g_ptr_array_add (self->records, rec);

  if (self->records->len > OE_UNDO_STACK_MAX_DEPTH)
    g_ptr_array_remove_index (self->records, 0);

  self->cursor = self->records->len;
  fire_changed (self);
}

/* ------------------------------------------------------------------ */
/* Recorder helpers: mutate through the model, then record.            */
/* ------------------------------------------------------------------ */

static void
record_insert (OeUndoStack *stack, OeProject *project, guint track_index, const OeClip *inserted)
{
  /* The mutator sorted the clip into position order; recover the
   * resulting index by matching the inserted tuple. */
  const guint count = oe_project_get_clip_count (project, track_index);

  for (guint i = 0; i < count; i++)
    {
      OeClip clip;

      if (!oe_project_get_clip (project, track_index, i, &clip))
        break;

      if (clip.media_ref == inserted->media_ref && clip.position_us == inserted->position_us
          && clip.source_in_us == inserted->source_in_us
          && clip.source_out_us == inserted->source_out_us)
        {
          OeUndoRecord *rec = record_new (
              OE_UNDO_OP_INSERT, g_strdup_printf ("Insert clip %u on track %u", i, track_index),
              track_index, i);

          clip_value_store (&rec->clip, inserted); /* private keyframe store */
          stack_push (stack, rec);
          return;
        }
    }

  /* Unreachable while the model contract holds (the insert just
     succeeded on this thread); degrade to an unrecorded edit rather
     than storing a wrong positional record. */
  oe_log (OE_LOG_LEVEL_WARNING, "undo: inserted clip not found on track %u; edit not recorded",
          track_index);
}

gboolean
oe_edit_insert_clip (OeProject *project, OeUndoStack *stack, guint track_index, const OeClip *clip,
                     GError **error)
{
  g_return_val_if_fail (clip != NULL, FALSE);

  if (!oe_project_insert_clip (project, track_index, clip->media_ref, clip->position_us,
                               clip->source_in_us, clip->source_out_us, error))
    return FALSE;

  if (stack != NULL)
    record_insert (stack, project, track_index, clip);

  return TRUE;
}

gboolean
oe_edit_remove_clip (OeProject *project, OeUndoStack *stack, guint track_index, guint clip_index,
                     GError **error)
{
  OeClip removed = { 0 };

  /* A successful remove implies these indices were in range, so the
     pre-read below succeeded too (same range checks); recording can
     trust @removed. */
  const gboolean have_clip
      = stack != NULL && oe_project_get_clip (project, track_index, clip_index, &removed);

  /* The capture aliases the live store; a private copy must exist
   * before the mutator frees the original clip. */
  if (have_clip)
    clip_capture (&removed);

  if (!oe_project_remove_clip (project, track_index, clip_index, error))
    {
      if (have_clip)
        clip_capture_clear (&removed);
      return FALSE;
    }

  if (stack != NULL)
    {
      if (!have_clip)
        {
          oe_log (OE_LOG_LEVEL_WARNING,
                  "undo: removed clip %u on track %u could not be captured; "
                  "edit not recorded",
                  clip_index, track_index);
          return TRUE;
        }

      OeUndoRecord *rec
          = record_new (OE_UNDO_OP_DELETE,
                        g_strdup_printf ("Delete clip %u on track %u", clip_index, track_index),
                        track_index, clip_index);

      rec->clip = removed; /* captured visual moves with the record */
      stack_push (stack, rec);
    }

  return TRUE;
}

gboolean
oe_edit_ripple_remove_clip (OeProject *project, OeUndoStack *stack, guint track_index,
                            guint clip_index, GError **error)
{
  OeClip removed = { 0 };
  GArray *shifts = NULL;
  GArray *reanchors = NULL;

  /* Capture the pre-state before the first mutation: the primary copy
   * and, for every downstream clip, its record-time identity. */
  if (stack != NULL)
    {
      if (!oe_project_get_clip (project, track_index, clip_index, &removed))
        {
          /* Same range checks the mutator runs; report as BAD_CLIP so
           * the caller sees the typed error instead of a warning. */
          g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP,
                       "ripple delete: no clip %u on track %u", clip_index, track_index);
          return FALSE;
        }

      /* The capture aliases the live store; a private copy must exist
       * before the mutator frees the original clip. */
      clip_capture (&removed);

      shifts = g_array_new (FALSE, FALSE, sizeof (OeRippleShift));
      reanchors = g_array_new (FALSE, FALSE, sizeof (OeTransitionReanchor));
    }

  /* Sub-step 1: the primary removal (renumbers downstream indices by
   * one — the shift entries below record both generations). */
  if (!oe_project_remove_clip (project, track_index, clip_index, error))
    {
      g_clear_pointer (&shifts, g_array_unref);
      g_clear_pointer (&reanchors, g_array_unref);
      if (stack != NULL)
        clip_capture_clear (&removed);
      return FALSE;
    }

  if (stack != NULL)
    {
      const gint64 shift_us = removed.source_out_us - removed.source_in_us;
      const guint count = oe_project_get_clip_count (project, track_index);

      /* Sub-step 2: shift the SUFFIX — the clips that sat to the
       * primary's right — left by the primary's duration, ascending
       * index order. Post-removal the first suffix clip has been
       * renumbered into the primary's old slot, so the suffix starts
       * at index clip_index; clips before it keep their positions.
       * Moving clip i left cannot overlap its not-yet-moved right
       * neighbour (equal shifts preserve pairwise gaps), so every
       * intermediate state stays valid. Index k here is the
       * post-removal index of the (clip_index + 1 + (k - clip_index))th
       * clip. A move failure is unreachable for a contract-abiding
       * model; the record keeps only the shifts that landed, so undo
       * still restores exactly what this action did. */
      for (guint k = clip_index; k < count; k++)
        {
          OeClip suffix = { 0 };
          OeRippleShift entry;

          if (!oe_project_get_clip (project, track_index, k, &suffix))
            break;

          entry.pre_index = k + 1;
          entry.post_index = k;
          entry.pre_position_us = suffix.position_us;
          entry.post_position_us = suffix.position_us - shift_us;

          if (!oe_project_move_clip (project, track_index, k, entry.post_position_us, error))
            {
              oe_log (OE_LOG_LEVEL_ERROR,
                      "ripple delete: suffix move %u failed (%s); recording partial shift", k,
                      (*error)->message);
              g_error_free (*error);
              *error = NULL;
              break;
            }

          g_array_append_vals (shifts, &entry, 1);
        }

      /* Sub-step 3: re-anchor the track's transitions whose boundary
       * moved with the ripple, through the validated mutator. A
       * boundary the ripple destroyed (no adjacent pair at the new
       * time) is skipped: the transition degrades to a straight cut
       * at composite time instead of being fixed up here. */
      const guint transition_count = oe_project_get_transition_count (project);

      for (guint t = 0; t < transition_count; t++)
        {
          OeTransition transition;

          if (!oe_project_get_transition (project, t, &transition))
            continue;
          if (transition.track_index != track_index)
            continue;
          if (transition.at_us <= removed.position_us)
            continue; /* boundary ahead of the removed clip's start persists in place */

          const gint64 pre_at_us = transition.at_us;
          const gint64 post_at_us = transition.at_us - shift_us;
          GError *reanchor_error = NULL;

          if (!oe_project_move_transition (project, t, post_at_us, &reanchor_error))
            {
              oe_log (OE_LOG_LEVEL_WARNING,
                      "ripple delete: transition %u boundary %lld not re-anchorable to %lld (%s); "
                      "degrades to a cut",
                      t, (long long) pre_at_us, (long long) post_at_us, reanchor_error->message);
              g_error_free (reanchor_error);
              continue;
            }

          const OeTransitionReanchor entry = {
            .index = t,
            .pre_at_us = pre_at_us,
            .post_at_us = post_at_us,
          };

          g_array_append_vals (reanchors, &entry, 1);
        }

      OeUndoRecord *rec = record_new (
          OE_UNDO_OP_RIPPLE_DELETE,
          g_strdup_printf ("Ripple delete clip %u on track %u", clip_index, track_index),
          track_index, clip_index);

      rec->clip = removed; /* captured visual moves with the record */
      rec->ripple_shifts = shifts;
      rec->transition_reanchors = reanchors;
      stack_push (stack, rec);
    }

  return TRUE;
}

gboolean
oe_edit_move_clip (OeProject *project, OeUndoStack *stack, guint track_index, guint clip_index,
                   gint64 position_us, GError **error)
{
  OeClip before = { 0 };

  const gboolean have_clip
      = stack != NULL && oe_project_get_clip (project, track_index, clip_index, &before);

  /* The capture aliases the live store; a private copy must exist
   * before the mutator runs. */
  if (have_clip)
    clip_capture (&before);

  if (!oe_project_move_clip (project, track_index, clip_index, position_us, error))
    {
      if (have_clip)
        clip_capture_clear (&before);
      return FALSE;
    }

  if (stack != NULL)
    {
      if (!have_clip)
        {
          oe_log (OE_LOG_LEVEL_WARNING,
                  "undo: moved clip %u on track %u could not be captured; "
                  "edit not recorded",
                  clip_index, track_index);
          return TRUE;
        }

      OeUndoRecord *rec = record_new (
          OE_UNDO_OP_MOVE, g_strdup_printf ("Move clip %u on track %u", clip_index, track_index),
          track_index, clip_index);

      rec->clip = before; /* captured visual moves with the record */
      rec->old_a_us = before.position_us;
      rec->new_a_us = position_us;
      stack_push (stack, rec);
    }

  return TRUE;
}

gboolean
oe_edit_trim_clip (OeProject *project, OeUndoStack *stack, guint track_index, guint clip_index,
                   gint64 source_in_us, gint64 source_out_us, GError **error)
{
  OeClip before = { 0 };

  const gboolean have_clip
      = stack != NULL && oe_project_get_clip (project, track_index, clip_index, &before);

  /* The capture aliases the live store; a private copy must exist
   * before the mutator runs. */
  if (have_clip)
    clip_capture (&before);

  if (!oe_project_trim_clip (project, track_index, clip_index, source_in_us, source_out_us, error))
    {
      if (have_clip)
        clip_capture_clear (&before);
      return FALSE;
    }

  if (stack != NULL)
    {
      if (!have_clip)
        {
          oe_log (OE_LOG_LEVEL_WARNING,
                  "undo: trimmed clip %u on track %u could not be captured; "
                  "edit not recorded",
                  clip_index, track_index);
          return TRUE;
        }

      OeUndoRecord *rec = record_new (
          OE_UNDO_OP_TRIM, g_strdup_printf ("Trim clip %u on track %u", clip_index, track_index),
          track_index, clip_index);

      rec->clip = before; /* captured visual moves with the record */
      rec->old_a_us = before.source_in_us;
      rec->old_b_us = before.source_out_us;
      rec->new_a_us = source_in_us;
      rec->new_b_us = source_out_us;
      stack_push (stack, rec);
    }

  return TRUE;
}

/* The visual recorders share the stroke assembly: mutate through the
 * validated mutator, then record ONE VISUAL record restoring the
 * stroke baseline. A zero-delta stroke mutates the model (idempotent)
 * but records nothing — history never carries a no-op step. */
static gboolean
record_visual_stroke (OeProject *project, OeUndoStack *stack, guint track_index, guint clip_index,
                      const OeClipVisual *old_visual, const OeClipVisual *new_visual,
                      GError **error)
{
  OeClip before = { 0 };

  if (stack == NULL)
    return oe_project_set_clip_visual (project, track_index, clip_index, new_visual, error);

  if (!oe_project_get_clip (project, track_index, clip_index, &before))
    return FALSE;

  /* The capture aliases the live store; a private copy must exist
   * before the mutator can release the original. */
  clip_capture (&before);

  if (!oe_project_set_clip_visual (project, track_index, clip_index, new_visual, error))
    {
      clip_capture_clear (&before);
      return FALSE;
    }

  if (oe_clip_visual_equal (old_visual, new_visual))
    {
      clip_capture_clear (&before);
      return TRUE; /* zero-delta stroke: the model already holds the state */
    }

  OeUndoRecord *rec = record_new (
      OE_UNDO_OP_VISUAL, g_strdup_printf ("Visual clip %u on track %u", clip_index, track_index),
      track_index, clip_index);

  rec->clip = before;
  /* The undo payload is the STROKE baseline, not the project state at
   * record time — a previewed stroke leaves the model at its last
   * preview, and undo must restore where the stroke began. The
   * captured store from @before is released and replaced by the
   * baseline's own deep copy. */
  oe_clip_visual_clear (&rec->clip.visual);
  visual_value_store (&rec->clip.visual, old_visual);
  visual_value_store (&rec->new_visual, new_visual);
  stack_push (stack, rec);
  return TRUE;
}

gboolean
oe_edit_set_clip_visual (OeProject *project, OeUndoStack *stack, guint track_index,
                         guint clip_index, const OeClipVisual *visual, GError **error)
{
  g_return_val_if_fail (visual != NULL, FALSE);

  OeClip before = { 0 };

  if (stack != NULL && !oe_project_get_clip (project, track_index, clip_index, &before))
    return FALSE;

  /* The capture aliases the live store; a private copy must exist
   * before the mutator can release the original. */
  if (stack != NULL)
    clip_capture (&before);

  if (!oe_project_set_clip_visual (project, track_index, clip_index, visual, error))
    {
      if (stack != NULL)
        clip_capture_clear (&before);
      return FALSE;
    }

  if (stack != NULL && !oe_clip_visual_equal (&before.visual, visual))
    {
      OeUndoRecord *rec
          = record_new (OE_UNDO_OP_VISUAL,
                        g_strdup_printf ("Visual clip %u on track %u", clip_index, track_index),
                        track_index, clip_index);

      rec->clip = before; /* baseline visual moves with the record */
      visual_value_store (&rec->new_visual, visual);
      stack_push (stack, rec);
    }
  else if (stack != NULL)
    {
      clip_capture_clear (&before);
    }

  return TRUE;
}

/* Shared keyframe-stroke recorder: the validated mutator has ALREADY
 * run when this is called; @before is the deep-captured pre-stroke
 * visual. The post state is read back from the project (the mutator
 * owns the new keyframe store), and one OE_UNDO_OP_VISUAL record is
 * pushed unless the stroke changed nothing. */
static gboolean
record_keyframe_stroke (OeProject *project, OeUndoStack *stack, guint track_index, guint clip_index,
                        OeClip *before, const gchar *property_name, GError **error)
{
  OeClip after = { 0 };

  if (!oe_project_get_clip (project, track_index, clip_index, &after))
    {
      /* The mutation landed; only the read-back failed — keep the
       * state, lose the history entry. */
      g_clear_error (error);
      clip_capture_clear (before);
      return TRUE;
    }

  if (oe_clip_visual_equal (&before->visual, &after.visual))
    {
      clip_capture_clear (before);
      /* `after` aliases the live store (borrowed getter result): never
       * unref it — the model still owns that array. */
      return TRUE; /* zero-delta stroke leaves no history entry */
    }

  OeUndoRecord *rec = record_new (
      OE_UNDO_OP_VISUAL,
      g_strdup_printf ("Keyframe %s on clip %u (track %u)", property_name, clip_index, track_index),
      track_index, clip_index);

  rec->clip = *before; /* baseline visual moves with the record */
  /* visual_value_store deep-copies out of the borrowed `after`; the
   * model keeps its own array, so `after` is never cleared. */
  visual_value_store (&rec->new_visual, &after.visual);
  stack_push (stack, rec);
  return TRUE;
}

gboolean
oe_edit_set_clip_keyframe (OeProject *project, OeUndoStack *stack, guint track_index,
                           guint clip_index, OeKeyframeProperty property, gint64 time_us,
                           gint32 value, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (project), FALSE);

  OeClip before = { 0 };

  if (stack != NULL && !oe_project_get_clip (project, track_index, clip_index, &before))
    return FALSE;

  /* The capture aliases the live store; a private copy must exist
   * before the mutator can release the original. */
  if (stack != NULL)
    clip_capture (&before);

  if (!oe_project_set_clip_keyframe (project, track_index, clip_index, property, time_us, value,
                                     error))
    {
      if (stack != NULL)
        clip_capture_clear (&before);
      return FALSE;
    }

  if (stack == NULL)
    return TRUE;

  return record_keyframe_stroke (project, stack, track_index, clip_index, &before,
                                 oe_keyframe_property_get_name (property), error);
}

gboolean
oe_edit_remove_clip_keyframe (OeProject *project, OeUndoStack *stack, guint track_index,
                              guint clip_index, OeKeyframeProperty property, gint64 time_us,
                              GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (project), FALSE);

  OeClip before = { 0 };

  if (stack != NULL && !oe_project_get_clip (project, track_index, clip_index, &before))
    return FALSE;

  if (stack != NULL)
    clip_capture (&before);

  if (!oe_project_remove_clip_keyframe (project, track_index, clip_index, property, time_us, error))
    {
      if (stack != NULL)
        clip_capture_clear (&before);
      return FALSE;
    }

  if (stack == NULL)
    return TRUE;

  return record_keyframe_stroke (project, stack, track_index, clip_index, &before,
                                 oe_keyframe_property_get_name (property), error);
}

gboolean
oe_edit_set_clip_visual_with_old (OeProject *project, OeUndoStack *stack, guint track_index,
                                  guint clip_index, const OeClipVisual *old_visual,
                                  const OeClipVisual *new_visual, GError **error)
{
  g_return_val_if_fail (old_visual != NULL && new_visual != NULL, FALSE);

  return record_visual_stroke (project, stack, track_index, clip_index, old_visual, new_visual,
                               error);
}

/* Shared clip-audio stroke recorder (Phase 10 Wave A): mutates to
 * @new_audio and records ONE #OE_UNDO_OP_CLIP_AUDIO record restoring
 * @old_audio — the stroke baseline, immune to preview mutations in
 * between. The payload owns no memory, so no deep copies are needed;
 * the aliased keyframe store in the captured `before` still requires
 * clip_capture() before the mutator can release the original. A
 * zero-delta stroke records nothing. */
static gboolean
record_clip_audio_stroke (OeProject *project, OeUndoStack *stack, guint track_index,
                          guint clip_index, const OeClipAudio *old_audio,
                          const OeClipAudio *new_audio, GError **error)
{
  OeClip before = { 0 };

  if (stack == NULL)
    return oe_project_set_clip_audio (project, track_index, clip_index, new_audio, error);

  if (!oe_project_get_clip (project, track_index, clip_index, &before))
    return FALSE;

  clip_capture (&before);

  if (!oe_project_set_clip_audio (project, track_index, clip_index, new_audio, error))
    {
      clip_capture_clear (&before);
      return FALSE;
    }

  if (oe_clip_audio_equal (old_audio, new_audio))
    {
      clip_capture_clear (&before);
      return TRUE; /* zero-delta stroke: the model already holds the state */
    }

  OeUndoRecord *rec = record_new (
      OE_UNDO_OP_CLIP_AUDIO, g_strdup_printf ("Audio clip %u on track %u", clip_index, track_index),
      track_index, clip_index);

  rec->clip = before;
  /* The undo payload is the STROKE baseline, not the project state at
   * record time — a previewed stroke leaves the model at its last
   * preview, and undo must restore where the stroke began. */
  rec->clip.audio = *old_audio;
  rec->new_clip_audio = *new_audio;
  stack_push (stack, rec);
  return TRUE;
}

/* Shared track-audio stroke recorder (Phase 10 Wave A): mutates to
 * @new_audio and records ONE #OE_UNDO_OP_TRACK_AUDIO record restoring
 * @old_audio. The payload is track-indexed (keyed by track_index
 * alone) and memory-free. A zero-delta stroke records nothing. */
static gboolean
record_track_audio_stroke (OeProject *project, OeUndoStack *stack, guint track_index,
                           const OeTrackAudio *old_audio, const OeTrackAudio *new_audio,
                           GError **error)
{
  OeTrackAudio before = { 0 };

  if (stack == NULL)
    return oe_project_set_track_audio (project, track_index, new_audio, error);

  if (!oe_project_get_track_audio (project, track_index, &before))
    return FALSE;

  if (!oe_project_set_track_audio (project, track_index, new_audio, error))
    return FALSE;

  if (oe_track_audio_equal (old_audio, new_audio))
    return TRUE; /* zero-delta stroke: the model already holds the state */

  OeUndoRecord *rec
      = record_new (OE_UNDO_OP_TRACK_AUDIO, g_strdup_printf ("Audio track %u", track_index),
                    track_index, 0); /* track-indexed payload: no clip identity */

  rec->old_track_audio = *old_audio;
  rec->new_track_audio = *new_audio;
  stack_push (stack, rec);
  return TRUE;
}

gboolean
oe_edit_set_clip_audio (OeProject *project, OeUndoStack *stack, guint track_index, guint clip_index,
                        const OeClipAudio *audio, GError **error)
{
  g_return_val_if_fail (audio != NULL, FALSE);

  OeClip before = { 0 };

  if (stack != NULL && !oe_project_get_clip (project, track_index, clip_index, &before))
    return FALSE;

  /* The capture aliases the live keyframe store; a private copy must
   * exist before the mutator can release the original. */
  if (stack != NULL)
    clip_capture (&before);

  if (!oe_project_set_clip_audio (project, track_index, clip_index, audio, error))
    {
      if (stack != NULL)
        clip_capture_clear (&before);
      return FALSE;
    }

  if (stack != NULL && !oe_clip_audio_equal (&before.audio, audio))
    {
      OeUndoRecord *rec
          = record_new (OE_UNDO_OP_CLIP_AUDIO,
                        g_strdup_printf ("Audio clip %u on track %u", clip_index, track_index),
                        track_index, clip_index);

      rec->clip = before; /* baseline audio moves with the record */
      rec->new_clip_audio = *audio;
      stack_push (stack, rec);
    }
  else if (stack != NULL)
    {
      clip_capture_clear (&before); /* zero-delta: no record took ownership */
    }

  return TRUE;
}

gboolean
oe_edit_set_clip_audio_with_old (OeProject *project, OeUndoStack *stack, guint track_index,
                                 guint clip_index, const OeClipAudio *old_audio,
                                 const OeClipAudio *new_audio, GError **error)
{
  g_return_val_if_fail (old_audio != NULL && new_audio != NULL, FALSE);

  return record_clip_audio_stroke (project, stack, track_index, clip_index, old_audio, new_audio,
                                   error);
}

gboolean
oe_edit_set_track_audio (OeProject *project, OeUndoStack *stack, guint track_index,
                         const OeTrackAudio *audio, GError **error)
{
  g_return_val_if_fail (audio != NULL, FALSE);

  OeTrackAudio before = { 0 };

  if (stack != NULL && !oe_project_get_track_audio (project, track_index, &before))
    return FALSE;

  if (!oe_project_set_track_audio (project, track_index, audio, error))
    return FALSE;

  if (stack != NULL && !oe_track_audio_equal (&before, audio))
    {
      OeUndoRecord *rec
          = record_new (OE_UNDO_OP_TRACK_AUDIO, g_strdup_printf ("Audio track %u", track_index),
                        track_index, 0); /* track-indexed payload: no clip identity */

      rec->old_track_audio = before;
      rec->new_track_audio = *audio;
      stack_push (stack, rec);
    }

  return TRUE;
}

gboolean
oe_edit_set_track_audio_with_old (OeProject *project, OeUndoStack *stack, guint track_index,
                                  const OeTrackAudio *old_audio, const OeTrackAudio *new_audio,
                                  GError **error)
{
  g_return_val_if_fail (old_audio != NULL && new_audio != NULL, FALSE);

  return record_track_audio_stroke (project, stack, track_index, old_audio, new_audio, error);
}

gboolean
oe_edit_set_clip_generator (OeProject *project, OeUndoStack *stack, guint track_index,
                            guint clip_index, const OeClipGenerator *generator, GError **error)
{
  g_return_val_if_fail (generator != NULL, FALSE);

  OeClip before = { 0 };

  if (stack != NULL && !oe_project_get_clip (project, track_index, clip_index, &before))
    return FALSE;

  /* The capture aliases the live generator text; a private copy must
   * exist before the mutator can release the original. */
  if (stack != NULL)
    clip_capture (&before);

  if (!oe_project_set_clip_generator (project, track_index, clip_index, generator, error))
    {
      if (stack != NULL)
        clip_capture_clear (&before);
      return FALSE;
    }

  if (stack != NULL && !oe_clip_generator_equal (&before.generator, generator))
    {
      OeUndoRecord *rec
          = record_new (OE_UNDO_OP_GENERATOR,
                        g_strdup_printf ("Title clip %u on track %u", clip_index, track_index),
                        track_index, clip_index);

      rec->clip = before; /* baseline generator (owned text) moves with the record */
      oe_clip_generator_copy (&rec->new_generator, generator);
      stack_push (stack, rec);
    }
  else if (stack != NULL)
    {
      clip_capture_clear (&before); /* zero-delta: no record took ownership */
    }

  return TRUE;
}

gboolean
oe_edit_set_clip_generator_with_old (OeProject *project, OeUndoStack *stack, guint track_index,
                                     guint clip_index, const OeClipGenerator *old_generator,
                                     const OeClipGenerator *new_generator, GError **error)
{
  g_return_val_if_fail (old_generator != NULL && new_generator != NULL, FALSE);

  return record_generator_stroke (project, stack, track_index, clip_index, old_generator,
                                  new_generator, error);
}

gboolean
oe_edit_set_clip_key (OeProject *project, OeUndoStack *stack, guint track_index, guint clip_index,
                      const OeClipKey *key, GError **error)
{
  g_return_val_if_fail (key != NULL, FALSE);

  OeClip before = { 0 };

  if (stack != NULL && !oe_project_get_clip (project, track_index, clip_index, &before))
    return FALSE;

  if (stack != NULL)
    clip_capture (&before); /* aliases the keyframe store, like audio */

  if (!oe_project_set_clip_key (project, track_index, clip_index, key, error))
    {
      if (stack != NULL)
        clip_capture_clear (&before);
      return FALSE;
    }

  if (stack != NULL && !oe_clip_key_equal (&before.key, key))
    {
      OeUndoRecord *rec = record_new (
          OE_UNDO_OP_CLIP_KEY, g_strdup_printf ("Key clip %u on track %u", clip_index, track_index),
          track_index, clip_index);

      rec->clip = before; /* baseline key moves with the record */
      rec->new_key = *key;
      stack_push (stack, rec);
    }
  else if (stack != NULL)
    {
      clip_capture_clear (&before); /* zero-delta: no record took ownership */
    }

  return TRUE;
}

gboolean
oe_edit_set_clip_key_with_old (OeProject *project, OeUndoStack *stack, guint track_index,
                               guint clip_index, const OeClipKey *old_key, const OeClipKey *new_key,
                               GError **error)
{
  g_return_val_if_fail (old_key != NULL && new_key != NULL, FALSE);

  return record_key_stroke (project, stack, track_index, clip_index, old_key, new_key, error);
}

/* Shared generator stroke recorder (Phase 11 Wave A): mutates to
 * @new_generator and records ONE #OE_UNDO_OP_GENERATOR record
 * restoring @old_generator — the stroke baseline, immune to preview
 * mutations in between. The payload OWNS its text (the visual
 * precedent): both generations are deep-copied into the record. A
 * zero-delta stroke records nothing. */
static gboolean
record_generator_stroke (OeProject *project, OeUndoStack *stack, guint track_index,
                         guint clip_index, const OeClipGenerator *old_generator,
                         const OeClipGenerator *new_generator, GError **error)
{
  OeClip before = { 0 };

  if (stack == NULL)
    return oe_project_set_clip_generator (project, track_index, clip_index, new_generator, error);

  if (!oe_project_get_clip (project, track_index, clip_index, &before))
    return FALSE;

  clip_capture (&before);

  if (!oe_project_set_clip_generator (project, track_index, clip_index, new_generator, error))
    {
      clip_capture_clear (&before);
      return FALSE;
    }

  if (oe_clip_generator_equal (old_generator, new_generator))
    {
      clip_capture_clear (&before);
      return TRUE; /* zero-delta stroke: the model already holds the state */
    }

  OeUndoRecord *rec = record_new (
      OE_UNDO_OP_GENERATOR, g_strdup_printf ("Title clip %u on track %u", clip_index, track_index),
      track_index, clip_index);

  rec->clip = before;
  /* The undo payload is the STROKE baseline, not the project state at
   * record time — a previewed stroke leaves the model at its last
   * preview, and undo must restore where the stroke began. */
  oe_clip_generator_copy (&rec->clip.generator, old_generator);
  oe_clip_generator_copy (&rec->new_generator, new_generator);
  stack_push (stack, rec);
  return TRUE;
}

/* Shared chroma-key stroke recorder (Phase 11 Wave A): mutates to
 * @new_key and records ONE #OE_UNDO_OP_CLIP_KEY record restoring
 * @old_key. The payload is memory-free (the audio precedent). A
 * zero-delta stroke records nothing. */
static gboolean
record_key_stroke (OeProject *project, OeUndoStack *stack, guint track_index, guint clip_index,
                   const OeClipKey *old_key, const OeClipKey *new_key, GError **error)
{
  OeClip before = { 0 };

  if (stack == NULL)
    return oe_project_set_clip_key (project, track_index, clip_index, new_key, error);

  if (!oe_project_get_clip (project, track_index, clip_index, &before))
    return FALSE;

  clip_capture (&before);

  if (!oe_project_set_clip_key (project, track_index, clip_index, new_key, error))
    {
      clip_capture_clear (&before);
      return FALSE;
    }

  if (oe_clip_key_equal (old_key, new_key))
    {
      clip_capture_clear (&before);
      return TRUE; /* zero-delta stroke: the model already holds the state */
    }

  OeUndoRecord *rec = record_new (
      OE_UNDO_OP_CLIP_KEY, g_strdup_printf ("Key clip %u on track %u", clip_index, track_index),
      track_index, clip_index);

  rec->clip = before;
  rec->clip.key = *old_key;
  rec->new_key = *new_key;
  stack_push (stack, rec);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* History application: inverse replay through the model mutators.     */
/* ------------------------------------------------------------------ */

/* Replay a composite RIPPLE_DELETE inverse: shift the suffix back
 * right (descending index order — moving clip i right cannot overlap
 * its not-yet-moved left neighbour, and the already-restored right
 * neighbour leaves room), then re-insert the primary into its freed
 * slot. Every sub-step runs through the model's validated mutators. */
static gboolean
apply_ripple_undo (OeProject *project, const OeUndoRecord *rec, GError **error)
{
  const GArray *shifts = rec->ripple_shifts;
  const gint64 shift_us = rec->clip.source_out_us - rec->clip.source_in_us;

  g_return_val_if_fail (shifts != NULL, FALSE);

  for (gsize k = shifts->len; k > 0; k--)
    {
      const OeRippleShift *entry = &g_array_index (shifts, OeRippleShift, k - 1);

      if (!oe_project_move_clip (project, rec->track_index, entry->post_index,
                                 entry->post_position_us + shift_us, error))
        return FALSE;
    }

  /* The primary goes back BEFORE the transition re-anchors: a
   * boundary at the removed clip's trailing edge only exists once the
   * clip itself is re-inserted. After suffix + insert the sequence is
   * exactly the pre-ripple state, so every recorded pre_at boundary is
   * valid (strict: a failure here is a real replay bug). */
  if (!oe_project_insert_clip (project, rec->track_index, rec->clip.media_ref,
                               rec->clip.position_us, rec->clip.source_in_us,
                               rec->clip.source_out_us, error))
    return FALSE;

  if (rec->transition_reanchors != NULL)
    {
      for (gsize k = 0; k < rec->transition_reanchors->len; k++)
        {
          const OeTransitionReanchor *entry
              = &g_array_index (rec->transition_reanchors, OeTransitionReanchor, k);

          if (!oe_project_move_transition (project, entry->index, entry->pre_at_us, error))
            return FALSE;
        }
    }

  return TRUE;
}

/* Replay a composite RIPPLE_DELETE forward: remove the primary, then
 * re-shift the suffix left in ascending index order — the record
 * path's own orderings, so redo reproduces the post-state exactly. */
static gboolean
apply_ripple_redo (OeProject *project, const OeUndoRecord *rec, GError **error)
{
  const GArray *shifts = rec->ripple_shifts;

  g_return_val_if_fail (shifts != NULL, FALSE);

  if (!oe_project_remove_clip (project, rec->track_index, rec->clip_index, error))
    return FALSE;

  for (gsize k = 0; k < shifts->len; k++)
    {
      const OeRippleShift *entry = &g_array_index (shifts, OeRippleShift, k);

      if (!oe_project_move_clip (project, rec->track_index, entry->post_index,
                                 entry->post_position_us, error))
        return FALSE;
    }

  /* Re-anchor the recorded transitions forward, mirroring the record
   * path's sub-step 3. */
  if (rec->transition_reanchors != NULL)
    {
      for (gsize k = 0; k < rec->transition_reanchors->len; k++)
        {
          const OeTransitionReanchor *entry
              = &g_array_index (rec->transition_reanchors, OeTransitionReanchor, k);

          if (!oe_project_move_transition (project, entry->index, entry->post_at_us, error))
            return FALSE;
        }
    }

  return TRUE;
}

static gboolean
apply_undo (OeProject *project, const OeUndoRecord *rec, GError **error)
{
  switch (rec->kind)
    {
    case OE_UNDO_OP_INSERT:
      return oe_project_remove_clip (project, rec->track_index, rec->clip_index, error);
    case OE_UNDO_OP_DELETE:
      return oe_project_insert_clip (project, rec->track_index, rec->clip.media_ref,
                                     rec->clip.position_us, rec->clip.source_in_us,
                                     rec->clip.source_out_us, error);
    case OE_UNDO_OP_MOVE:
      return oe_project_move_clip (project, rec->track_index, rec->clip_index, rec->old_a_us,
                                   error);
    case OE_UNDO_OP_TRIM:
      return oe_project_trim_clip (project, rec->track_index, rec->clip_index, rec->old_a_us,
                                   rec->old_b_us, error);
    case OE_UNDO_OP_VISUAL:
      return oe_project_set_clip_visual (project, rec->track_index, rec->clip_index,
                                         &rec->clip.visual, error);
    case OE_UNDO_OP_CLIP_AUDIO:
      return oe_project_set_clip_audio (project, rec->track_index, rec->clip_index,
                                        &rec->clip.audio, error);
    case OE_UNDO_OP_TRACK_AUDIO:
      return oe_project_set_track_audio (project, rec->track_index, &rec->old_track_audio, error);
    case OE_UNDO_OP_GENERATOR:
      return oe_project_set_clip_generator (project, rec->track_index, rec->clip_index,
                                            &rec->clip.generator, error);
    case OE_UNDO_OP_CLIP_KEY:
      return oe_project_set_clip_key (project, rec->track_index, rec->clip_index, &rec->clip.key,
                                      error);
    case OE_UNDO_OP_RIPPLE_DELETE:
      return apply_ripple_undo (project, rec, error);
    }

  g_assert_not_reached ();
}

static gboolean
apply_redo (OeProject *project, const OeUndoRecord *rec, GError **error)
{
  switch (rec->kind)
    {
    case OE_UNDO_OP_INSERT:
      return oe_project_insert_clip (project, rec->track_index, rec->clip.media_ref,
                                     rec->clip.position_us, rec->clip.source_in_us,
                                     rec->clip.source_out_us, error);
    case OE_UNDO_OP_DELETE:
      return oe_project_remove_clip (project, rec->track_index, rec->clip_index, error);
    case OE_UNDO_OP_MOVE:
      return oe_project_move_clip (project, rec->track_index, rec->clip_index, rec->new_a_us,
                                   error);
    case OE_UNDO_OP_TRIM:
      return oe_project_trim_clip (project, rec->track_index, rec->clip_index, rec->new_a_us,
                                   rec->new_b_us, error);
    case OE_UNDO_OP_VISUAL:
      return oe_project_set_clip_visual (project, rec->track_index, rec->clip_index,
                                         &rec->new_visual, error);
    case OE_UNDO_OP_CLIP_AUDIO:
      return oe_project_set_clip_audio (project, rec->track_index, rec->clip_index,
                                        &rec->new_clip_audio, error);
    case OE_UNDO_OP_TRACK_AUDIO:
      return oe_project_set_track_audio (project, rec->track_index, &rec->new_track_audio, error);
    case OE_UNDO_OP_GENERATOR:
      return oe_project_set_clip_generator (project, rec->track_index, rec->clip_index,
                                            &rec->new_generator, error);
    case OE_UNDO_OP_CLIP_KEY:
      return oe_project_set_clip_key (project, rec->track_index, rec->clip_index, &rec->new_key,
                                      error);
    case OE_UNDO_OP_RIPPLE_DELETE:
      return apply_ripple_redo (project, rec, error);
    }

  g_assert_not_reached ();
}

GQuark
oe_undo_stack_error_quark (void)
{
  return g_quark_from_static_string ("oe-undo-stack-error");
}

static gboolean
history_apply (OeUndoStack *self, OeProject *project, gboolean undo, const OeUndoRecord **out,
               GError **error)
{
  /* Undo peeks at the record below the cursor, redo at the one above;
     the cursor moves only after the mutator accepted the inverse, so a
     typed rejection leaves the failed op current. */
  const gboolean has_record = undo ? self->cursor > 0 : self->cursor < self->records->len;

  if (!has_record)
    {
      g_set_error (error, OE_UNDO_STACK_ERROR, OE_UNDO_STACK_ERROR_EMPTY, "nothing to %s",
                   undo ? "undo" : "redo");
      return FALSE;
    }

  const OeUndoRecord *rec
      = g_ptr_array_index (self->records, undo ? self->cursor - 1 : self->cursor);

  if (!(undo ? apply_undo (project, rec, error) : apply_redo (project, rec, error)))
    return FALSE;

  self->cursor += undo ? -1 : 1;

  if (out != NULL)
    *out = rec;

  fire_changed (self);
  return TRUE;
}

gboolean
oe_undo_stack_undo (OeUndoStack *stack, OeProject *project, const OeUndoRecord **out,
                    GError **error)
{
  g_return_val_if_fail (stack != NULL, FALSE);
  g_return_val_if_fail (OE_IS_PROJECT (project), FALSE);

  return history_apply (stack, project, TRUE, out, error);
}

gboolean
oe_undo_stack_redo (OeUndoStack *stack, OeProject *project, const OeUndoRecord **out,
                    GError **error)
{
  g_return_val_if_fail (stack != NULL, FALSE);
  g_return_val_if_fail (OE_IS_PROJECT (project), FALSE);

  return history_apply (stack, project, FALSE, out, error);
}

/* The auto-pause interplay: a PLAYING session holds a deep copy taken
 * at play/seek time, so a history op applied under it would play a
 * stale sequence. Pausing first guarantees the next play re-copies the
 * mutated project (oe_playback_session.h). */
static void
pause_if_playing (OePlaybackSession *session)
{
  if (session != NULL && oe_playback_session_get_state (session) == OE_PLAYBACK_PLAYING)
    oe_playback_session_pause (session);
}

gboolean
oe_undo_stack_undo_with_session (OeUndoStack *stack, OeProject *project, OePlaybackSession *session,
                                 const OeUndoRecord **out, GError **error)
{
  pause_if_playing (session);
  return oe_undo_stack_undo (stack, project, out, error);
}

gboolean
oe_undo_stack_redo_with_session (OeUndoStack *stack, OeProject *project, OePlaybackSession *session,
                                 const OeUndoRecord **out, GError **error)
{
  pause_if_playing (session);
  return oe_undo_stack_redo (stack, project, out, error);
}
