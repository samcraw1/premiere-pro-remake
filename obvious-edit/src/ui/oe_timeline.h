/* oe_timeline.h — the timeline widget (Phase 4).
 *
 * A GtkDrawingArea that renders the project's sequence and turns
 * pointer gestures into model mutations. Layer rules for this widget:
 *
 *   - It is the first production consumer of the project observer:
 *     oe_timeline_set_project() wires oe_project_set_observer(), and
 *     every repaint works on a fresh oe_project_get_sequence() deep
 *     copy. The widget never holds a reference into the model it
 *     could mutate around.
 *   - It never edits clips directly: press arms a drag (move, trim-in,
 *     trim-out, or playhead), motion previews the CLAMPED result via
 *     the GTK-free layout core, and release commits through
 *     oe_project_move_clip() / oe_project_trim_clip(). A typed
 *     rejection is reported through the report func and the preview
 *     snaps back — the model stays authoritative and always legal.
 *   - The playhead and zoom are widget-session state (no model
 *     fields, no serialization): Phase 5 owns the playback clock.
 *
 * Media lookups (missing hatch, audio strip) ride a resolve callback
 * supplied by the window: the widget stays free of library or probe
 * types, and the window answers from the session it owns.
 */

#pragma once

#include <gtk/gtk.h>

#include "../core/oe_project.h"

/* Forward reference: the Phase 6 edit recorder (src/app). The widget
 * holds a weak pointer — the window owns the stack. */
typedef struct _OeUndoStack OeUndoStack;

G_BEGIN_DECLS

#define OE_TYPE_TIMELINE (oe_timeline_get_type ())
G_DECLARE_FINAL_TYPE (OeTimeline, oe_timeline, OE, TIMELINE, GtkDrawingArea)

/* Session default for a still's screen duration (uniform-duration
 * rule: a still's source range encodes screen time, so insertion
 * picks a conventional 5 s). */
#define OE_TIMELINE_DEFAULT_STILL_US 5000000

OeTimeline *oe_timeline_new (void);

/**
 * oe_timeline_set_project:
 * @project: (transfer none) or NULL to detach
 *
 * The widget keeps a weak pointer and registers itself as the
 * project's observer; passing NULL detaches both. Replacing a project
 * detaches the old observer first, so ownership never doubles up.
 */
void oe_timeline_set_project (OeTimeline *timeline, OeProject *project);

/**
 * OeTimelineMediaInfo: what the draw pass needs to know about one
 * media reference.
 * @missing: TRUE when the file is unavailable (hatched, refuses trims)
 * @has_audio: draw the audio strip (AV media with an audio stream)
 * @is_still: still badge instead of a duration bar
 */
typedef struct
{
  gboolean missing;
  gboolean has_audio;
  gboolean is_still;
} OeTimelineMediaInfo;

/**
 * OeTimelineResolveFunc:
 * @media_ref: the clip's media reference
 * @info: fills the fields; unknown refs answer missing = TRUE
 * @user_data: context supplied at connect time
 */
typedef void (*OeTimelineResolveFunc) (guint media_ref, OeTimelineMediaInfo *info,
                                       gpointer user_data);

void oe_timeline_set_resolve_func (OeTimeline *timeline, OeTimelineResolveFunc func,
                                   gpointer user_data);

/**
 * OeTimelineReportFunc:
 * @message: one-line user-facing text (rejections, missing media)
 * @user_data: context supplied at connect time
 */
typedef void (*OeTimelineReportFunc) (const gchar *message, gpointer user_data);

void oe_timeline_set_report_func (OeTimeline *timeline, OeTimelineReportFunc func,
                                  gpointer user_data);

/**
 * OeTimelinePlayheadFunc:
 * @playhead_us: the playhead in microseconds
 * @user_data: context supplied at connect time
 *
 * Notified when the user moves the playhead by hand (ruler click or
 * drag) so the playback session can seek. Positions pushed down from
 * the session clock do not loop back through this callback.
 */
typedef void (*OeTimelinePlayheadFunc) (gint64 playhead_us, gpointer user_data);

void oe_timeline_set_playhead_func (OeTimeline *timeline, OeTimelinePlayheadFunc func,
                                    gpointer user_data);

/**
 * oe_timeline_set_undo_stack:
 * @stack: (transfer none) or NULL to detach
 *
 * Weak pointer to the session's edit history (the window owns the
 * stack). Drag commits then run through the oe_edit_* recorder helpers
 * so every accepted move/trim lands in history; NULL keeps the widget
 * editing through the model unrecorded.
 */
void oe_timeline_set_undo_stack (OeTimeline *timeline, OeUndoStack *stack);

/** Selected clip indices; FALSE when the selection is empty. */
gboolean oe_timeline_get_selection (OeTimeline *timeline, guint *track_index, guint *clip_index);

/** Drops the selection (after a delete, before a session swap). */
void oe_timeline_clear_selection (OeTimeline *timeline);

/** Session zoom, in pixels per microsecond. */
gdouble oe_timeline_get_zoom (OeTimeline *timeline);
/** Zoom steps for the view commands: double/halve around the widget
 * center, clamped to the session zoom floor/ceiling. */
void oe_timeline_zoom_in (OeTimeline *timeline);
void oe_timeline_zoom_out (OeTimeline *timeline);

/** Session playhead in microseconds (ruler click / insert advance;
 * never persisted — Phase 5 owns the clock). */
gint64 oe_timeline_get_playhead (OeTimeline *timeline);
void oe_timeline_set_playhead (OeTimeline *timeline, gint64 playhead_us);

G_END_DECLS
