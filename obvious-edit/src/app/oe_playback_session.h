/* oe_playback_session.h — the GTK-free playback clock (Phase 5).
 *
 * Owns everything time-shaped about transport:
 *
 *   - a wall-clock-anchored clock: the position is always
 *     anchor_position + (monotonic_now - anchor_time), integer µs end to
 *     end, re-anchored (never re-accumulated) on play, pause/resume, and
 *     seek — pause/resume drift accounting is the anchor reset;
 *   - a stopped/paused/playing state machine with a pure clip→source
 *     mapping per the model's only defined mapping (source in/out);
 *   - tick(): advances the clock, feeds audio, decodes video, fires the
 *     observer, and returns the next deadline in monotonic µs so the
 *     owner schedules pacing with a plain GSource at that deadline;
 *   - an injectable time source: production leaves it unset and reads
 *     g_get_monotonic_time(); tests install a virtual clock through
 *     oe_playback_session_set_time_source() so deadline and drift
 *     assertions are deterministic, fast, and Valgrind-clean.
 *   - end-of-sequence stop computed from a deep copy of the project's
 *     sequence (max clip end), never from live widgets.
 *
 * GTK never appears here. The owner is the main thread: every entry
 * point runs there, and the media worker's deliveries are invoked onto
 * the main context, so no locking is needed.
 *
 * Audio device discipline: the stream is opened on first play and closed
 * on free. Pause/stop flush the queue; play from stopped or resume from
 * paused re-requests audio from the current position, so every stream
 * start begins with a fresh anchor and an empty queue. With no device
 * (graceful no-audio path) or SDL's dummy driver, the wall clock alone
 * paces playback.
 */

#pragma once

#include <glib.h>

#include "../core/oe_project.h"
#include "../media/oe_media_playback.h"

G_BEGIN_DECLS

/**
 * OePlaybackState: the three transport states.
 */
typedef enum
{
  OE_PLAYBACK_STOPPED,
  OE_PLAYBACK_PLAYING,
  OE_PLAYBACK_PAUSED,
} OePlaybackState;

/**
 * OePlaybackEvent: transport events beyond position/state changes.
 * @OE_PLAYBACK_EVENT_MISSING_MEDIA_SKIPPED: a clip's media failed to
 *     decode during playback; the transport continues (detail carries
 *     the reason).
 * @OE_PLAYBACK_EVENT_END_OF_SEQUENCE: the clock reached the sequence end
 *     and the session stopped with the playhead parked there.
 * @OE_PLAYBACK_EVENT_NOTHING_TO_PLAY: play() was called on an empty
 *     sequence; the state did not change and no error is raised.
 */
typedef enum
{
  OE_PLAYBACK_EVENT_MISSING_MEDIA_SKIPPED,
  OE_PLAYBACK_EVENT_END_OF_SEQUENCE,
  OE_PLAYBACK_EVENT_NOTHING_TO_PLAY,
} OePlaybackEvent;

typedef struct _OePlaybackSession OePlaybackSession;

/**
 * OePlaybackTimeSourceFunc: reads the current time on a monotonic scale,
 * in integer µs. Production sessions leave the time source unset — the
 * session then uses g_get_monotonic_time(). Tests install a virtual
 * clock so wall-cadence assertions never depend on real sleeps.
 */
typedef gint64 (*OePlaybackTimeSourceFunc) (gpointer user_data);

/**
 * OePlaybackNotifyFunc: observer fired on the main context after each
 * tick, seek, or state change. Borrowed arguments — copy if keeping.
 */
typedef void (*OePlaybackNotifyFunc) (const OePlaybackSession *session, gint64 position_us,
                                      OePlaybackState state, gpointer user_data);

/**
 * OePlaybackFrameFunc: delivered on the main context when a video frame
 * is ready for the program monitor. Ownership of @frame transfers to the
 * callback (NULL @frame means no video is scheduled at the position —
 * clear the monitor).
 */
typedef void (*OePlaybackFrameFunc) (const OePlaybackSession *session, OePlaybackVideoFrame *frame,
                                     gpointer user_data);

/**
 * OePlaybackEventFunc: transport events (see #OePlaybackEvent). @detail
 * is borrowed and may be NULL.
 */
typedef void (*OePlaybackEventFunc) (const OePlaybackSession *session, OePlaybackEvent event,
                                     const gchar *detail, gpointer user_data);

/**
 * OePlaybackMapping: result of the pure clip→source mapping — the only
 * mapping the model defines (source in/out). @source_us is clamped into
 * the owning clip's [source_in, source_out).
 */
typedef struct
{
  gboolean active;
  guint track_index;
  guint clip_index;
  gint64 source_us;
} OePlaybackMapping;

/**
 * oe_playback_session_map:
 * @sequence: a sequence snapshot (deep copy)
 * @kind: which track kind to map
 * @position_us: sequence position to map
 * @out: receives the mapping
 *
 * The topmost (highest-index) track of @kind that contains
 * @position_us wins, mirroring the layering order in the model.
 *
 * Returns: TRUE when a clip covers @position_us.
 */
gboolean oe_playback_session_map (const OeSequence *sequence, OeTrackKind kind, gint64 position_us,
                                  OePlaybackMapping *out);

/**
 * oe_playback_session_new:
 * @project: borrowed project; the owner frees the session before the
 *     project. A deep copy of the sequence is taken at play/seek time.
 *
 * Returns: (transfer full): the session; free with
 *     oe_playback_session_free() BEFORE the media subsystem shuts down.
 */
OePlaybackSession *oe_playback_session_new (const OeProject *project);

/**
 * oe_playback_session_free:
 * @session: (transfer full): the session, or NULL (a no-op)
 *
 * Cancels outstanding audio requests, drains and joins the media worker,
 * closes the audio stream, and frees the sequence copy. Call on the main
 * thread before oe_ffmpeg_shutdown().
 */
void oe_playback_session_free (OePlaybackSession *session);

void oe_playback_session_set_observer (OePlaybackSession *session, OePlaybackNotifyFunc notify,
                                       gpointer user_data);
void oe_playback_session_set_frame_func (OePlaybackSession *session, OePlaybackFrameFunc frame_func,
                                         gpointer user_data);
void oe_playback_session_set_event_func (OePlaybackSession *session, OePlaybackEventFunc event_func,
                                         gpointer user_data);

/**
 * oe_playback_session_set_time_source:
 * @time_func: the monotonic µs reader, or NULL to restore the production
 *     default (g_get_monotonic_time())
 * @user_data: passed to @time_func
 *
 * Swaps where the session reads "now". The wall-clock-anchored design is
 * unchanged — only the source of the monotonic reading is injectable.
 */
void oe_playback_session_set_time_source (OePlaybackSession *session,
                                          OePlaybackTimeSourceFunc time_func, gpointer user_data);

/**
 * oe_playback_session_play:
 * @error: return location for a #GError, or NULL to ignore
 *
 * STOPPED → PLAYING from the parked position; PAUSED → PLAYING with a
 * fresh anchor; PLAYING → no-op. An empty sequence stays stopped and
 * fires #OE_PLAYBACK_EVENT_NOTHING_TO_PLAY instead of failing.
 *
 * Returns: TRUE (playback never fails hard; missing media is skipped).
 */
gboolean oe_playback_session_play (OePlaybackSession *session, GError **error);

/**
 * oe_playback_session_pause:
 *
 * PLAYING → PAUSED with the position frozen at the pause point (drift
 * accounting resets: the resume re-anchors). Other states are a no-op.
 */
void oe_playback_session_pause (OePlaybackSession *session);

/**
 * oe_playback_session_stop:
 *
 * Any state → STOPPED, parking the playhead at the current position and
 * dropping queued audio. Stopping a stopped session is a no-op.
 */
void oe_playback_session_stop (OePlaybackSession *session);

/**
 * oe_playback_session_seek:
 *
 * Moves the playhead to @position_us (clamped to [0, sequence_end]) and
 * refreshes the sequence copy. While playing this is a clock reset plus
 * decoder flush: the session re-anchors, flushes queued audio, and
 * re-requests audio from the new position.
 */
void oe_playback_session_seek (OePlaybackSession *session, gint64 position_us);

/**
 * oe_playback_session_tick:
 *
 * Advances the clock, corrects drift against consumed audio on real
 * devices, feeds the audio queue, decodes the video frame at the new
 * position, fires the observer, and — at sequence end — stops with the
 * playhead parked.
 *
 * Returns: the next deadline in monotonic µs while playing (schedule a
 *     GSource there), or "now" when not playing.
 */
gint64 oe_playback_session_tick (OePlaybackSession *session);

gint64 oe_playback_session_get_position (const OePlaybackSession *session);
OePlaybackState oe_playback_session_get_state (const OePlaybackSession *session);

/**
 * oe_playback_session_get_sequence_end:
 *
 * The cached end of the sequence (max clip end) from the latest deep
 * copy; 0 for an empty sequence.
 */
gint64 oe_playback_session_get_sequence_end (const OePlaybackSession *session);

G_END_DECLS
