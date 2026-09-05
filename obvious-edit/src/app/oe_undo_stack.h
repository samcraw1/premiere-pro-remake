/* oe_undo_stack.h — GTK-free command-object undo/redo history (Phase 6).
 *
 * The stack owns one #OeUndoRecord per accepted edit: the minimal inverse
 * payload (an owned clip copy for insert/delete, old/new bounds for
 * move/trim) replayed through the project model's own validated mutators.
 * Nothing here includes gtk.h — GLib only, headless-testable.
 *
 * THE SOLE-PATH STRICT-LIFO INVARIANT. Clips have no stable identity:
 * a clip IS its positional index (track_index, clip_index), ordered by
 * position_us (oe_project.h). Positional records are therefore correct
 * only while both of these hold:
 *
 *   1. Sole path — during a session, every project mutation flows
 *      through the oe_edit_* recorder helpers below. An edit that
 *      bypasses the recorder shifts indices behind the stack's back and
 *      invalidates the records that follow it.
 *   2. Strict LIFO — undo/redo replay records in exact reverse/forward
 *      order, and the stack is cleared whenever the project is replaced
 *      (oe_undo_stack_clear). No random-access history, no cross-project
 *      records.
 *
 * If the invariant ever breaks anyway, apply-time replay meets the
 * model's typed validation (OVERLAP/BAD_RANGE/BAD_TRACK/BAD_CLIP): the
 * failing op is reported with a typed GError and the stack position is
 * left unchanged, so history degrades to rejections — never to silent
 * corruption.
 *
 * Snapshot escape hatch. Command objects are the default because they
 * are small, labeled, and re-validated by the model on every replay.
 * For a future mutator that is genuinely hard to invert (a hypothetical
 * remove-track, for example), the documented alternative is a
 * stack-of-states mode: deep-copy the whole OeSequence per edit
 * (oe_sequence_copy) and restore wholesale. Do not build it until such
 * a mutator exists — it costs O(project) memory per edit and cannot
 * carry per-edit labels.
 *
 * Layer note: src/app is the GTK-free service layer
 * (architecture.md). The playback-session-aware entry points live here
 * too so the auto-pause interplay is testable headlessly; the module
 * borrows the session for one pause call and keeps no reference to it.
 */

#pragma once

#include <glib.h>

#include "../core/oe_project.h"

/* Forward reference: the auto-pause entry points take a borrowed
 * session pointer. Duplicating the typedef keeps oe_undo_stack.h
 * independent of the playback/media headers (identical typedef
 * redeclaration is valid C11+); the .c includes the real header. */
typedef struct _OePlaybackSession OePlaybackSession;

G_BEGIN_DECLS

/**
 * OeUndoStack: an opaque bounded history of edit records.
 *
 * Created with oe_undo_stack_new(), freed with oe_undo_stack_free().
 * All entry points are main-thread-only, like the model it replays
 * into.
 */
typedef struct _OeUndoStack OeUndoStack;

/**
 * OE_UNDO_STACK_ERROR: error domain for history-level failures (an
 * empty history). Replay failures carry the model's own
 * #OE_PROJECT_ERROR domain unchanged.
 */
#define OE_UNDO_STACK_ERROR (oe_undo_stack_error_quark ())

GQuark oe_undo_stack_error_quark (void);

/**
 * OeUndoStackError:
 * @OE_UNDO_STACK_ERROR_EMPTY: undo/redo was asked for with no record
 *     on that side of the history cursor.
 */
typedef enum
{
  OE_UNDO_STACK_ERROR_EMPTY,
} OeUndoStackError;

/** Maximum records held; pushing one more drops the oldest. */
#define OE_UNDO_STACK_MAX_DEPTH 100

/**
 * OeUndoOpKind: the scoped edit operations. Every kind maps 1:1 onto
 * an existing project mutator, in both directions — except the
 * composite ripple record, which replays a fixed sequence of existing
 * mutator calls (see oe_edit_ripple_remove_clip).
 *
 * @OE_UNDO_OP_INSERT: a clip was added at (track, index); undo removes
 *     it, redo re-inserts the stored copy.
 * @OE_UNDO_OP_DELETE: a clip was removed; undo re-inserts the stored
 *     copy, redo removes it again.
 * @OE_UNDO_OP_MOVE: a clip's position changed; undo/redo move it back
 *     and forth (@old_a_us / @new_a_us). @old_b_us / @new_b_us unused.
 * @OE_UNDO_OP_TRIM: a clip's source range changed; undo/redo trim back
 *     to @old_a_us/@old_b_us and @new_a_us/@new_b_us (in/out).
 * @OE_UNDO_OP_RIPPLE_DELETE: a clip was removed AND its same-track
 *     downstream suffix shifted left by the removed clip's duration,
 *     recorded as ONE composite record (one user action = one undo
 *     step). Undo restores the suffix (descending index order) and
 *     re-inserts the primary; redo removes the primary and re-shifts
 *     (ascending index order). Transitions on the affected track whose
 *     boundary moved with the ripple are re-anchored through the
 *     validated transition mutator as one more replay sub-step (Phase
 *     9 Wave B); boundaries the ripple destroyed are skipped at record
 *     time — the transition then degrades to a straight cut at
 *     composite time, so the history stays exact. The sub-step
 *     orderings keep every intermediate state inside the model's
 *     typed validation.
 * @OE_UNDO_OP_VISUAL: a clip's picture geometry/opacity changed
 *     (Phase 9 Wave A); undo restores the pre-stroke #OeClipVisual,
 *     redo re-applies the post-stroke one. One inspector stroke = one
 *     record, whatever the control count involved.
 * @OE_UNDO_OP_CLIP_AUDIO: a clip's audio gain/pan changed (Phase 10
 *     Wave A); undo restores the pre-stroke #OeClipAudio, redo
 *     re-applies the post-stroke one. One inspector stroke = one
 *     record. Same record/replay machinery as VISUAL; the payload is
 *     memory-free, so replay runs through the validated clip-audio
 *     mutator.
 * @OE_UNDO_OP_TRACK_AUDIO: an audio track's volume/pan/mute/solo
 *     changed (Phase 10 Wave A); undo restores the pre-stroke
 *     #OeTrackAudio, redo re-applies the post-stroke one. NEW payload
 *     shape: keyed by @track_index ALONE — audio state belongs to the
 *     track, and the model rejects audio state on video tracks, so no
 *     sentinel clip index is ever needed.
 */
typedef enum
{
  OE_UNDO_OP_INSERT,
  OE_UNDO_OP_DELETE,
  OE_UNDO_OP_MOVE,
  OE_UNDO_OP_TRIM,
  OE_UNDO_OP_RIPPLE_DELETE,
  OE_UNDO_OP_VISUAL,
  OE_UNDO_OP_CLIP_AUDIO,
  OE_UNDO_OP_TRACK_AUDIO,
} OeUndoOpKind;

/**
 * OeRippleShift: one suffix clip's pre/post identity inside a
 * composite #OE_UNDO_OP_RIPPLE_DELETE record. Both index generations
 * are stored explicitly because oe_project_remove_clip() renumbers
 * downstream clips: @pre_* is the identity at record time, @post_* the
 * identity after the primary removal and shift.
 */
typedef struct
{
  gint64 pre_position_us;
  gint64 post_position_us;
  guint pre_index;
  guint post_index;
} OeRippleShift;

/**
 * OeTransitionReanchor: one transition's pre/post boundary inside a
 * composite #OE_UNDO_OP_RIPPLE_DELETE record (Phase 9 Wave B). The
 * boundary re-anchors by the ripple's shift delta through the
 * validated mutator; the transition index is stable because no
 * transition add/remove participates in a ripple.
 */
typedef struct
{
  guint index;
  gint64 pre_at_us;
  gint64 post_at_us;
} OeTransitionReanchor;

/**
 * OeUndoRecord: one immutable command object, owned by the stack.
 * @label: status-bar text ("Move clip 2 on track 0"); owned by the
 *     record, borrowed by readers.
 * @clip: owned deep copy of the edited clip (insert/delete payload).
 *     Clips own keyframe stores since Phase 9 Wave B, so this is a
 *     REAL deep copy, never a struct copy of the live clip:
 *     clip_value_store()/clip_capture() replace the aliased store
 *     pointer with a private one, and record_free releases it exactly
 *     once. The VISUAL kind reuses the same field as its undo
 *     baseline.
 */
typedef struct
{
  OeUndoOpKind kind;
  gchar *label;
  guint track_index;
  guint clip_index; /* positional identity at record time */
  OeClip clip;
  gint64 old_a_us;
  gint64 old_b_us;
  gint64 new_a_us;
  gint64 new_b_us;
  OeClipVisual new_visual; /* VISUAL only: the post-stroke state */
  OeClipAudio new_clip_audio; /* CLIP_AUDIO only: the post-stroke state */
  OeTrackAudio old_track_audio; /* TRACK_AUDIO only: the pre-stroke state */
  OeTrackAudio new_track_audio; /* TRACK_AUDIO only: the post-stroke state */

  /* RIPPLE_DELETE only: owned array of OeRippleShift, one entry per
   * shifted suffix clip ordered by pre_index ascending; NULL for every
   * other kind. An empty array is a degenerate ripple (no downstream
   * clips) — undo/redo reduce to the plain delete/insert pair. */
  GArray *ripple_shifts;

  /* RIPPLE_DELETE only: owned array of OeTransitionReanchor, one
   * entry per transition re-anchored by the shift delta; NULL for
   * every other kind. Boundaries the ripple destroyed are absent —
   * those transitions degrade to straight cuts instead. */
  GArray *transition_reanchors;
} OeUndoRecord;

/**
 * OeUndoChangedFunc: fired after every history transition — a record
 * was pushed, history was cleared, or an undo/redo was applied. The
 * window wires this to command enablement.
 */
typedef void (*OeUndoChangedFunc) (gboolean can_undo, gboolean can_redo, gpointer user_data);

/**
 * oe_undo_stack_new:
 *
 * Returns: (transfer full): an empty stack with a depth of
 *     #OE_UNDO_STACK_MAX_DEPTH (oldest record dropped beyond it).
 */
OeUndoStack *oe_undo_stack_new (void);

/**
 * oe_undo_stack_free:
 * @stack: (transfer full): the stack, or NULL (a no-op)
 *
 * Frees every record. The stack never owned project or session memory.
 */
void oe_undo_stack_free (OeUndoStack *stack);

/**
 * oe_undo_stack_set_changed_func:
 * @func: change sink, or NULL to clear
 * @user_data: passed back with every call
 *
 * Single change slot, following the project's observer idiom. Never
 * fires at set time — only on real transitions.
 */
void oe_undo_stack_set_changed_func (OeUndoStack *stack, OeUndoChangedFunc func,
                                     gpointer user_data);

/**
 * oe_undo_stack_clear:
 *
 * Drops all records (both branches) and fires the changed func. The
 * project-replace boundary: call it on New/Open so history never
 * crosses a project.
 */
void oe_undo_stack_clear (OeUndoStack *stack);

gboolean oe_undo_stack_can_undo (const OeUndoStack *stack);
gboolean oe_undo_stack_can_redo (const OeUndoStack *stack);
/** Total records held across both branches (capped at the depth). */
guint oe_undo_stack_get_size (const OeUndoStack *stack);

/* Recorder helpers: perform the mutation through the project model,
 * then record the inverse payload — only on success. A typed-rejected
 * mutator call records nothing. @stack may be NULL to edit without
 * recording (the mutation still happens through the model). */

/**
 * oe_edit_insert_clip:
 * @clip: placement to insert; position/source/media_ref must be filled
 *
 * Inserts through oe_project_insert_clip() and records the resulting
 * position-ordered index by matching the inserted tuple.
 */
gboolean oe_edit_insert_clip (OeProject *project, OeUndoStack *stack, guint track_index,
                              const OeClip *clip, GError **error);

/** Removes through oe_project_remove_clip(), recording the clip copy. */
gboolean oe_edit_remove_clip (OeProject *project, OeUndoStack *stack, guint track_index,
                              guint clip_index, GError **error);

/**
 * oe_edit_ripple_remove_clip: ripple delete (Phase 7).
 *
 * Removes the clip at (@track_index, @clip_index) and shifts every
 * same-track downstream clip left by the removed clip's duration —
 * the removed clip's footprint is exactly the freed gap, so the suffix
 * lands flush where the deleted clip sat. Every sub-step goes through
 * the model's own validated mutators (remove, then move per suffix
 * clip, ascending index order); the whole action pushes ONE composite
 * #OE_UNDO_OP_RIPPLE_DELETE record, so one undo restores the exact
 * pre-state and one redo reproduces the post-state. With no downstream
 * clips the record degenerates to the plain delete payload. @stack may
 * be NULL to edit without recording (the physical ripple still
 * happens).
 */
gboolean oe_edit_ripple_remove_clip (OeProject *project, OeUndoStack *stack, guint track_index,
                                     guint clip_index, GError **error);

/** Moves through oe_project_move_clip(), recording old/new positions. */
gboolean oe_edit_move_clip (OeProject *project, OeUndoStack *stack, guint track_index,
                            guint clip_index, gint64 position_us, GError **error);

/** Trims through oe_project_trim_clip(), recording old/new bounds. */
gboolean oe_edit_trim_clip (OeProject *project, OeUndoStack *stack, guint track_index,
                            guint clip_index, gint64 source_in_us, gint64 source_out_us,
                            GError **error);

/**
 * oe_edit_set_clip_visual: visual edit (Phase 9 Wave A).
 *
 * Mutates through oe_project_set_clip_visual() and records ONE
 * #OE_UNDO_OP_VISUAL record whose old state is the project's visual
 * captured immediately before the mutation. Right for one-shot edits
 * (numeric entry); interactive strokes must call the _with_old
 * variant instead — a drag previews through unrecorded mutations, so
 * the pre-capture baseline here would be the last preview state, not
 * the stroke's start.
 */
gboolean oe_edit_set_clip_visual (OeProject *project, OeUndoStack *stack, guint track_index,
                                  guint clip_index, const OeClipVisual *visual, GError **error);

/**
 * oe_edit_set_clip_visual_with_old:
 * @old_visual: the pre-stroke baseline, captured when the stroke began
 * @new_visual: the post-stroke state
 *
 * Mutates to @new_visual through oe_project_set_clip_visual() and
 * records ONE #OE_UNDO_OP_VISUAL record restoring @old_visual — the
 * true stroke start, immune to how many preview mutations happened in
 * between. A @new_visual equal to @old_visual records nothing: a
 * zero-delta stroke leaves no history entry.
 */
gboolean oe_edit_set_clip_visual_with_old (OeProject *project, OeUndoStack *stack,
                                           guint track_index, guint clip_index,
                                           const OeClipVisual *old_visual,
                                           const OeClipVisual *new_visual, GError **error);

/**
 * oe_edit_set_clip_keyframe / oe_edit_remove_clip_keyframe: inspector
 * keyframe strokes (Phase 9 Wave B).
 *
 * Mutate through the validated keyframe mutators and record ONE
 * #OE_UNDO_OP_VISUAL record — a keyframe edit IS a visual-property
 * edit, so undo restores the whole pre-stroke visual (keyframe stores
 * included) and redo re-applies the post-stroke one. A stroke that
 * leaves the visual unchanged (e.g. replacing a key with an identical
 * value, removing a key that is not there) records nothing.
 */
gboolean oe_edit_set_clip_keyframe (OeProject *project, OeUndoStack *stack, guint track_index,
                                    guint clip_index, OeKeyframeProperty property, gint64 time_us,
                                    gint32 value, GError **error);
gboolean oe_edit_remove_clip_keyframe (OeProject *project, OeUndoStack *stack, guint track_index,
                                       guint clip_index, OeKeyframeProperty property,
                                       gint64 time_us, GError **error);

/**
 * oe_edit_set_clip_audio: clip-audio edit (Phase 10 Wave A).
 *
 * Mutates through oe_project_set_clip_audio() and records ONE
 * #OE_UNDO_OP_CLIP_AUDIO record whose old state is the clip's audio
 * captured immediately before the mutation. Right for one-shot edits
 * (numeric entry); interactive strokes must call the _with_old
 * variant instead — a drag previews through unrecorded mutations, so
 * the pre-capture baseline here would be the last preview state, not
 * the stroke's start.
 */
gboolean oe_edit_set_clip_audio (OeProject *project, OeUndoStack *stack, guint track_index,
                                 guint clip_index, const OeClipAudio *audio, GError **error);

/**
 * oe_edit_set_clip_audio_with_old:
 * @old_audio: the pre-stroke baseline, captured when the stroke began
 * @new_audio: the post-stroke state
 *
 * Mutates to @new_audio through oe_project_set_clip_audio() and
 * records ONE #OE_UNDO_OP_CLIP_AUDIO record restoring @old_audio —
 * the true stroke start, immune to how many preview mutations
 * happened in between. A @new_audio equal to @old_audio records
 * nothing: a zero-delta stroke leaves no history entry.
 */
gboolean oe_edit_set_clip_audio_with_old (OeProject *project, OeUndoStack *stack,
                                          guint track_index, guint clip_index,
                                          const OeClipAudio *old_audio,
                                          const OeClipAudio *new_audio, GError **error);

/**
 * oe_edit_set_track_audio: track-audio edit (Phase 10 Wave A).
 *
 * Mutates through oe_project_set_track_audio() and records ONE
 * #OE_UNDO_OP_TRACK_AUDIO record — the track-indexed payload shape
 * (no clip index). The plain variant captures the track's audio
 * immediately before the mutation; strokes use the _with_old variant.
 * Video tracks are rejected by the model mutator and record nothing.
 */
gboolean oe_edit_set_track_audio (OeProject *project, OeUndoStack *stack, guint track_index,
                                  const OeTrackAudio *audio, GError **error);

/**
 * oe_edit_set_track_audio_with_old: stroke variant of
 * oe_edit_set_track_audio(). Mutates to @new_audio and records ONE
 * #OE_UNDO_OP_TRACK_AUDIO record restoring @old_audio — the true
 * stroke start. A @new_audio equal to @old_audio records nothing: a
 * zero-delta stroke leaves no history entry.
 */
gboolean oe_edit_set_track_audio_with_old (OeProject *project, OeUndoStack *stack,
                                           guint track_index, const OeTrackAudio *old_audio,
                                           const OeTrackAudio *new_audio, GError **error);

/**
 * oe_undo_stack_undo:
 * @out: receives the applied record (borrowed — valid until the next
 *     stack mutation), or NULL to ignore
 *
 * Applies the newest undoable record's inverse through the model's
 * mutators. On a typed mutator rejection the stack position is left
 * unchanged (the failed op stays current) and @error carries the
 * model's #OE_PROJECT_ERROR.
 *
 * Returns: TRUE on success.
 */
gboolean oe_undo_stack_undo (OeUndoStack *stack, OeProject *project, const OeUndoRecord **out,
                             GError **error);

/**
 * oe_undo_stack_redo:
 *
 * Re-applies the newest undone record (the redo branch's oldest),
 * exactly like oe_undo_stack_undo() in reverse.
 */
gboolean oe_undo_stack_redo (OeUndoStack *stack, OeProject *project, const OeUndoRecord **out,
                             GError **error);

/**
 * oe_undo_stack_undo_with_session: auto-pause variant of
 * oe_undo_stack_undo(). A PLAYING @session is paused FIRST (the
 * session plays a deep copy taken at play/seek time, so a mutated
 * project only reaches playback on the next play, which re-copies) and
 * the history op is applied after. @session may be NULL.
 */
gboolean oe_undo_stack_undo_with_session (OeUndoStack *stack, OeProject *project,
                                          OePlaybackSession *session, const OeUndoRecord **out,
                                          GError **error);

/** Auto-pause variant of oe_undo_stack_redo(); @session may be NULL. */
gboolean oe_undo_stack_redo_with_session (OeUndoStack *stack, OeProject *project,
                                          OePlaybackSession *session, const OeUndoRecord **out,
                                          GError **error);

G_END_DECLS
