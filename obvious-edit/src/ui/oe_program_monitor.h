/* oe_program_monitor.h — the program-monitor drawing area (Phase 5).
 *
 * A GtkDrawingArea that renders owned RGBA frames delivered by the
 * playback session's frame callback, plus the off-happy-path states:
 * empty until the first frame, a hatch when media is missing. Deep-copy
 * draw discipline: the widget owns its pixel buffer exclusively; the
 * draw function reads it; nothing else holds a reference.
 */

#pragma once

#include <gtk/gtk.h>

#include "../media/oe_media_playback.h"

G_BEGIN_DECLS

#define OE_TYPE_PROGRAM_MONITOR (oe_program_monitor_get_type ())
G_DECLARE_FINAL_TYPE (OeProgramMonitor, oe_program_monitor, OE, PROGRAM_MONITOR, GtkDrawingArea)

/**
 * oe_program_monitor_new:
 *
 * Returns: (transfer full): a new program monitor.
 */
OeProgramMonitor *oe_program_monitor_new (void);

/**
 * oe_program_monitor_show_frame:
 * @frame: (transfer full): the frame to display and adopt, or NULL to
 *     clear back to the empty state (a gap or audio-only stretch).
 *
 * Adopts the frame's buffer (no copy); the monitor frees it when the
 * next frame arrives or the widget is destroyed.
 */
void oe_program_monitor_show_frame (OeProgramMonitor *monitor, OePlaybackVideoFrame *frame);

/**
 * oe_program_monitor_set_missing:
 * @missing: TRUE to draw the missing-media hatch over the monitor
 *
 * Cleared by the next delivered frame or oe_program_monitor_clear().
 */
void oe_program_monitor_set_missing (OeProgramMonitor *monitor, gboolean missing);

/**
 * oe_program_monitor_clear:
 *
 * Drops the current frame and missing state, returning to the empty
 * state shown before the first frame of a session.
 */
void oe_program_monitor_clear (OeProgramMonitor *monitor);

G_END_DECLS
