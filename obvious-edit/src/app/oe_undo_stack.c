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
  return rec;
}

static void
record_free (gpointer data)
{
  OeUndoRecord *rec = data;

  g_free (rec->label);
  g_free (rec);
}

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

          rec->clip = *inserted;
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

  if (!oe_project_remove_clip (project, track_index, clip_index, error))
    return FALSE;

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

      rec->clip = removed;
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

  if (!oe_project_move_clip (project, track_index, clip_index, position_us, error))
    return FALSE;

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

      rec->clip = before;
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

  if (!oe_project_trim_clip (project, track_index, clip_index, source_in_us, source_out_us, error))
    return FALSE;

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

      rec->clip = before;
      rec->old_a_us = before.source_in_us;
      rec->old_b_us = before.source_out_us;
      rec->new_a_us = source_in_us;
      rec->new_b_us = source_out_us;
      stack_push (stack, rec);
    }

  return TRUE;
}

/* ------------------------------------------------------------------ */
/* History application: inverse replay through the model mutators.     */
/* ------------------------------------------------------------------ */

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
