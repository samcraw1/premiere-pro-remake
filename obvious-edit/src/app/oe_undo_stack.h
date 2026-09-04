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
 * OeUndoOpKind: the four scoped edit operations. Every kind maps 1:1
 * onto an existing project mutator, in both directions.
 *
 * @OE_UNDO_OP_INSERT: a clip was added at (track, index); undo removes
 *     it, redo re-inserts the stored copy.
 * @OE_UNDO_OP_DELETE: a clip was removed; undo re-inserts the stored
 *     copy, redo removes it again.
 * @OE_UNDO_OP_MOVE: a clip's position changed; undo/redo move it back
 *     and forth (@old_a_us / @new_a_us). @old_b_us / @new_b_us unused.
 * @OE_UNDO_OP_TRIM: a clip's source range changed; undo/redo trim back
 *     to @old_a_us/@old_b_us and @new_a_us/@new_b_us (in/out).
 */
typedef enum
{
  OE_UNDO_OP_INSERT,
  OE_UNDO_OP_DELETE,
  OE_UNDO_OP_MOVE,
  OE_UNDO_OP_TRIM,
} OeUndoOpKind;

/**
 * OeUndoRecord: one immutable command object, owned by the stack.
 * @label: status-bar text ("Move clip 2 on track 0"); owned by the
 *     record, borrowed by readers.
 * @clip: owned deep copy of the edited clip (insert/delete payload —
 *     OeClip owns no memory, so the struct copy IS the deep copy).
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

/** Moves through oe_project_move_clip(), recording old/new positions. */
gboolean oe_edit_move_clip (OeProject *project, OeUndoStack *stack, guint track_index,
                            guint clip_index, gint64 position_us, GError **error);

/** Trims through oe_project_trim_clip(), recording old/new bounds. */
gboolean oe_edit_trim_clip (OeProject *project, OeUndoStack *stack, guint track_index,
                            guint clip_index, gint64 source_in_us, gint64 source_out_us,
                            GError **error);

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
