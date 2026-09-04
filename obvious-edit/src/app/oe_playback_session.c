/* oe_playback_session.c — the GTK-free playback clock (Phase 5).
 *
 * Time discipline: integer µs end to end. The position while playing is
 * always anchor_position + (monotonic_now - anchor_time); play, resume,
 * and seek RE-ANCHOR (fresh pair) rather than accumulating deltas, which
 * is the pause/resume drift accounting. On a real audio device the
 * consumed-audio position corrects the wall clock (slewed, with a snap
 * bound); on the dummy driver or with no device the wall clock alone
 * paces playback.
 *
 * Threading: every entry point runs on the main thread. The media
 * worker's chunk deliveries are invoked onto the main context, so no
 * lock is taken anywhere in this file.
 */

#include "oe_playback_session.h"

#include "../core/oe_time.h"
#include "../playback/oe_audio_output.h"
#include "oe_log.h"

/* Audio feed policy: keep the queue filled through position + lookahead,
 * one bounded request at a time. */
#define AUDIO_LOOKAHEAD_US G_GINT64_CONSTANT (250000)
#define AUDIO_REQUEST_SPAN_US G_GINT64_CONSTANT (1000000)
/* The dummy driver never consumes: cap what we push into the void. */
#define AUDIO_DUMMY_QUEUE_CAP_FRAMES (48000 * 2) /* ~2 s at 48 kHz stereo */

/* Drift correction: slew up to 1 ms per tick toward the consumed-audio
 * position; snap when the gap exceeds a frame (device drop or stall). */
#define DRIFT_SLEW_US 1000
#define DRIFT_SNAP_US G_GINT64_CONSTANT (40000)

struct _OePlaybackSession
{
  /* Borrowed: the owner frees the session before the project. */
  const OeProject *project;

  /* Deep copy taken at play()/seek() — end-of-sequence and mapping run
   * against this snapshot, never against live widgets. */
  OeSequence sequence;
  gboolean sequence_valid;
  gint64 sequence_end_us;

  OePlaybackState state;
  gint64 last_position_us; /* position while paused or stopped */

  /* Wall-clock anchor (playing only): position ↔ monotonic pair. */
  gint64 anchor_position_us;
  gint64 anchor_time_us;

  OePlaybackNotifyFunc notify;
  gpointer notify_data;
  OePlaybackFrameFunc frame_func;
  gpointer frame_data;
  OePlaybackEventFunc event_func;
  gpointer event_data;

  OeMediaPlaybackWorker *worker;
  guint audio_generation; /* bumped on every stream restart */

  OeAudioStream *stream;
  OeAudioDeviceInfo stream_info;
  gint64 audio_base_position_us;  /* sequence position when the queue was flushed */
  gint64 audio_pushed_frames;     /* frames accepted since the flush */
  gboolean audio_outstanding;     /* a decode request is in flight */
  gint64 audio_clip_position_us;  /* owning clip placement (sequence time) */
  gint64 audio_clip_source_in_us; /* owning clip source origin */
  gint64 audio_clip_end_seq_us;   /* sequence time where the clip ends */
  gchar *audio_failed_path;       /* media that failed this generation */

  OeMediaVideoDecoder *video_dec;
  gchar *video_dec_path;
  gchar *video_failed_path;        /* media that failed to open this run */
  gint64 video_frame_index;        /* sequence frame index last decoded */
  gboolean video_active;           /* a video clip covers the position */
  gboolean video_missing_reported; /* once per play run */
};

/* ------------------------------------------------------------------ */
/* Pure helpers                                                        */
/* ------------------------------------------------------------------ */

/* The model's only defined mapping: the topmost (highest-index) track of
 * @kind that contains the position wins; the source time is the clip's
 * source origin plus the offset, clamped into [source_in, source_out).
 * Seeks always respect clip source in/out — this function is where. */
gboolean
oe_playback_session_map (const OeSequence *sequence, OeTrackKind kind, gint64 position_us,
                         OePlaybackMapping *out)
{
  g_return_val_if_fail (sequence != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);

  if (sequence->tracks == NULL)
    {
      *out = (OePlaybackMapping) { FALSE, 0, 0, 0 };
      return FALSE;
    }

  for (guint t = sequence->tracks->len; t-- > 0;)
    {
      const OeTrack *track = g_ptr_array_index (sequence->tracks, t);

      if (track->kind != kind || track->clips == NULL)
        continue;

      for (guint c = 0; c < track->clips->len; c++)
        {
          const OeClip *clip = g_ptr_array_index (track->clips, c);
          const gint64 length = clip->source_out_us - clip->source_in_us;

          if (position_us < clip->position_us || position_us >= clip->position_us + length)
            continue;

          out->active = TRUE;
          out->track_index = t;
          out->clip_index = c;
          out->source_us = CLAMP (clip->source_in_us + (position_us - clip->position_us),
                                  clip->source_in_us, clip->source_out_us - 1);
          return TRUE;
        }
    }

  *out = (OePlaybackMapping) { FALSE, 0, 0, 0 };
  return FALSE;
}

static gint64
apply_drift_correction (gint64 wall_pos, gint64 audio_pos)
{
  const gint64 diff = audio_pos - wall_pos;

  if (diff > DRIFT_SNAP_US || diff < -DRIFT_SNAP_US)
    return audio_pos;
  if (diff > DRIFT_SLEW_US)
    return wall_pos + DRIFT_SLEW_US;
  if (diff < -DRIFT_SLEW_US)
    return wall_pos - DRIFT_SLEW_US;
  return audio_pos;
}

static gint64
frame_interval_us (const OePlaybackSession *self)
{
  const OeRational *rate = &self->sequence.frame_rate;

  if (rate->num <= 0 || rate->den <= 0)
    return G_GINT64_CONSTANT (20000);

  return CLAMP (G_GINT64_CONSTANT (1000000) * rate->den / rate->num, 1000,
                G_GINT64_CONSTANT (1000000));
}

/* Max clip end over all tracks — the end-of-sequence rule. */
static gint64
compute_sequence_end (const OeSequence *sequence)
{
  gint64 end = 0;

  if (sequence->tracks == NULL)
    return 0;

  for (guint t = 0; t < sequence->tracks->len; t++)
    {
      const OeTrack *track = g_ptr_array_index (sequence->tracks, t);

      if (track->clips == NULL)
        continue;

      for (guint c = 0; c < track->clips->len; c++)
        {
          const OeClip *clip = g_ptr_array_index (track->clips, c);
          end = MAX (end, clip->position_us + (clip->source_out_us - clip->source_in_us));
        }
    }

  return end;
}

/* ------------------------------------------------------------------ */
/* Observer plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
fire_notify (OePlaybackSession *self, gint64 position_us)
{
  if (self->notify != NULL)
    self->notify (self, position_us, self->state, self->notify_data);
}

static void
fire_event (OePlaybackSession *self, OePlaybackEvent event, const gchar *detail)
{
  if (self->event_func != NULL)
    self->event_func (self, event, detail, self->event_data);
}

/* ------------------------------------------------------------------ */
/* Sequence snapshot                                                   */
/* ------------------------------------------------------------------ */

static void
refresh_sequence (OePlaybackSession *self)
{
  /* oe_project_get_sequence replaces caller storage wholesale — it never
   * clears @out — so hand it zeroed storage: clear owns any previous
   * snapshot and is a no-op on the zeroed struct the instance starts with. */
  oe_sequence_clear (&self->sequence);

  oe_project_get_sequence ((OeProject *) self->project, &self->sequence);
  self->sequence_valid = TRUE;
  self->sequence_end_us = compute_sequence_end (&self->sequence);
}

/* ------------------------------------------------------------------ */
/* Audio: stream lifecycle, feeding, delivery                          */
/* ------------------------------------------------------------------ */

static void
submit_audio_request (OePlaybackSession *self, gint64 position_us)
{
  if (self->stream == NULL || self->worker == NULL)
    return;

  /* Map position + lookahead into the audio lane; gaps stay silent. */
  OePlaybackMapping map;
  const gint64 seq_from = position_us + AUDIO_LOOKAHEAD_US;

  if (!oe_playback_session_map (&self->sequence, OE_TRACK_AUDIO, seq_from, &map))
    return;

  const OeTrack *track = g_ptr_array_index (self->sequence.tracks, map.track_index);
  const OeClip *clip = g_ptr_array_index (track->clips, map.clip_index);
  gchar *path = oe_project_dup_media_path ((OeProject *) self->project, clip->media_ref);

  if (path == NULL)
    return;

  /* One failing file per generation: skip it, keep the transport going. */
  if (g_strcmp0 (path, self->audio_failed_path) == 0)
    {
      g_free (path);
      return;
    }

  const gint64 clip_length = clip->source_out_us - clip->source_in_us;
  const gint64 src_from = CLAMP (clip->source_in_us + (seq_from - clip->position_us),
                                 clip->source_in_us, clip->source_out_us);
  const gint64 src_to = MIN (clip->source_out_us, src_from + AUDIO_REQUEST_SPAN_US);

  if (src_to <= src_from)
    {
      g_free (path);
      return;
    }

  /* Record the owning clip so chunk deliveries can map source → sequence. */
  self->audio_clip_position_us = clip->position_us;
  self->audio_clip_source_in_us = clip->source_in_us;
  self->audio_clip_end_seq_us = clip->position_us + clip_length;
  self->audio_outstanding = TRUE;

  oe_log (OE_LOG_LEVEL_DEBUG, "audio request: '%s' [%lld, %lld) gen %u", path, (long long) src_from,
          (long long) src_to, self->audio_generation);
  oe_media_playback_worker_request (self->worker, path, src_from, src_to,
                                    self->stream_info.sample_rate, self->stream_info.channels,
                                    self->audio_generation);
  g_free (path);
}

/* Keep the queue filled through position + lookahead. Works unchanged on
 * the dummy driver: pushed-coverage advances with the wall clock, and the
 * dummy cap bounds how much is pushed into the void. */
static void
feed_audio (OePlaybackSession *self, gint64 position_us)
{
  if (self->stream == NULL || self->worker == NULL || self->audio_outstanding)
    return;

  if (self->stream_info.sample_rate <= 0)
    return;

  const gint64 pushed_through
      = self->audio_base_position_us
        + self->audio_pushed_frames * G_GINT64_CONSTANT (1000000) / self->stream_info.sample_rate;

  if (pushed_through >= position_us + AUDIO_LOOKAHEAD_US)
    return;

  if (self->stream_info.is_dummy
      && oe_audio_output_queued_frames (self->stream) > AUDIO_DUMMY_QUEUE_CAP_FRAMES)
    return;

  submit_audio_request (self, position_us);
}

static void
on_worker_audio (OePlaybackAudioChunk *chunk, const GError *error, gpointer user_data)
{
  OePlaybackSession *self = user_data;

  if (chunk == NULL && error == NULL)
    {
      /* Range exhausted: the feed loop may request more. */
      self->audio_outstanding = FALSE;
      return;
    }

  if (chunk == NULL)
    {
      /* Decode failed: report once per generation, continue without. */
      self->audio_outstanding = FALSE;
      if (self->audio_failed_path == NULL && error->message != NULL)
        {
          self->audio_failed_path = g_strdup (error->message);
          fire_event (self, OE_PLAYBACK_EVENT_MISSING_MEDIA_SKIPPED, error->message);
        }
      return;
    }

  if (chunk->generation != self->audio_generation)
    return; /* stale delivery — the worker's trampoline owns the chunk */

  /* Map the chunk's source time through the owning clip recorded at
   * request time, truncate at the clip's sequence end (source in/out is
   * the only mapping the model defines), and queue it. */
  const gint64 seq_start
      = self->audio_clip_position_us + (chunk->source_us - self->audio_clip_source_in_us);
  gint64 keep_frames = 0;

  if (seq_start < self->audio_clip_end_seq_us && chunk->sample_rate > 0)
    keep_frames = (self->audio_clip_end_seq_us - seq_start) * (gint64) chunk->sample_rate
                  / G_GINT64_CONSTANT (1000000);

  if (keep_frames > 0 && (gsize) keep_frames > chunk->n_frames)
    keep_frames = (gint64) chunk->n_frames;

  if (keep_frames > 0 && self->stream != NULL)
    {
      const gsize pushed
          = oe_audio_output_queue (self->stream, chunk->interleaved, (gsize) keep_frames);
      self->audio_pushed_frames += (gint64) pushed;

      if (pushed > 0)
        oe_log (OE_LOG_LEVEL_DEBUG, "audio queued: %zu frames, depth %zu", pushed,
                oe_audio_output_queued_frames (self->stream));
    }
  /* No free here: ownership stays with the worker's main-context
   * delivery, which frees the chunk after this callback returns. */
}

static void
halt_streaming (OePlaybackSession *self)
{
  self->audio_generation++;
  self->audio_outstanding = FALSE;

  if (self->worker != NULL)
    oe_media_playback_worker_cancel (self->worker);

  if (self->stream != NULL)
    {
      oe_audio_output_flush (self->stream);
      oe_audio_output_set_running (self->stream, FALSE);
    }
}

/* Every transition into PLAYING goes through here: fresh anchor, empty
 * queue, device running, audio requested from @position. */
static void
restart_streaming (OePlaybackSession *self, gint64 position_us)
{
  halt_streaming (self);
  g_clear_pointer (&self->audio_failed_path, g_free);

  if (self->stream != NULL)
    oe_audio_output_set_running (self->stream, TRUE);

  self->audio_base_position_us = position_us;
  self->audio_pushed_frames = 0;
  self->anchor_position_us = position_us;
  self->anchor_time_us = g_get_monotonic_time ();

  submit_audio_request (self, position_us);
}

static void
ensure_stream (OePlaybackSession *self)
{
  GError *error = NULL;

  if (self->stream != NULL)
    return;

  self->stream = oe_audio_output_open_stream (&self->stream_info, &error);

  if (self->stream == NULL)
    {
      /* Graceful no-audio path: the wall clock alone paces playback. */
      oe_log (OE_LOG_LEVEL_WARNING, "playing without audio: %s", error->message);
      g_error_free (error);
      self->stream_info.sample_rate = 0;
      self->stream_info.channels = 0;
      self->stream_info.is_dummy = TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* Video: decoder lifecycle, frame-at-position                         */
/* ------------------------------------------------------------------ */

#define VIDEO_DECODE_BOX_W 1280
#define VIDEO_DECODE_BOX_H 720

static void
close_video_decoder (OePlaybackSession *self)
{
  g_clear_pointer (&self->video_dec, oe_media_playback_video_free);
  g_clear_pointer (&self->video_dec_path, g_free);
  g_clear_pointer (&self->video_failed_path, g_free);
}

/* Decode and deliver the frame at @position, or clear the monitor when
 * no video is scheduled. Missing media reports once per run and the
 * transport continues. */
static void
update_video (OePlaybackSession *self, gint64 position_us)
{
  OePlaybackMapping map;
  const gboolean active
      = oe_playback_session_map (&self->sequence, OE_TRACK_VIDEO, position_us, &map);

  if (!active)
    {
      if (self->video_active)
        {
          self->video_active = FALSE;
          if (self->frame_func != NULL)
            self->frame_func (self, NULL, self->frame_data);
        }
      return;
    }

  const gint64 frame_index = oe_time_us_to_frame (position_us, self->sequence.frame_rate);

  if (self->video_active && frame_index == self->video_frame_index)
    return; /* same frame — stills decode once per source frame */

  const OeTrack *track = g_ptr_array_index (self->sequence.tracks, map.track_index);
  const OeClip *clip = g_ptr_array_index (track->clips, map.clip_index);
  gchar *path = oe_project_dup_media_path ((OeProject *) self->project, clip->media_ref);

  if (path == NULL)
    return;

  /* One failing file per play run: skip further opens, keep the
   * transport going (mirrors the audio guard). */
  if (g_strcmp0 (path, self->video_failed_path) == 0)
    {
      g_free (path);
      return;
    }

  if (self->video_dec == NULL || g_strcmp0 (self->video_dec_path, path) != 0)
    {
      GError *open_error = NULL;
      OeMediaVideoDecoder *dec = oe_media_playback_video_open (path, &open_error);

      if (dec == NULL)
        {
          if (!self->video_missing_reported)
            {
              self->video_missing_reported = TRUE;
              self->video_failed_path = g_strdup (path);
              fire_event (self, OE_PLAYBACK_EVENT_MISSING_MEDIA_SKIPPED, open_error->message);
            }
          g_error_free (open_error);
          g_free (path);
          return;
        }

      close_video_decoder (self);
      self->video_dec = dec;
      self->video_dec_path = g_strdup (path);
    }

  GError *decode_error = NULL;
  OePlaybackVideoFrame *frame = NULL;

  if (oe_media_playback_video_decode_at (self->video_dec, map.source_us, VIDEO_DECODE_BOX_W,
                                         VIDEO_DECODE_BOX_H, &frame, &decode_error))
    {
      self->video_active = TRUE;
      self->video_frame_index = frame_index;

      if (self->frame_func != NULL)
        self->frame_func (self, frame, self->frame_data); /* ownership transfers */
      else
        oe_playback_video_frame_free (frame);
    }
  else
    {
      if (!self->video_missing_reported)
        {
          self->video_missing_reported = TRUE;
          fire_event (self, OE_PLAYBACK_EVENT_MISSING_MEDIA_SKIPPED, decode_error->message);
        }
      g_error_free (decode_error);
    }

  g_free (path);
}

/* ------------------------------------------------------------------ */
/* Clock                                                               */
/* ------------------------------------------------------------------ */

static gint64
compute_position (OePlaybackSession *self, gint64 now_us)
{
  gint64 raw = self->anchor_position_us + (now_us - self->anchor_time_us);

  if (raw < 0)
    raw = 0;

  /* Real device only: the dummy driver never consumes, so its queue
   * depth is not a pacing signal. */
  if (self->stream == NULL || self->stream_info.is_dummy)
    return raw;

  const gsize queued = oe_audio_output_queued_frames (self->stream);
  const gint64 consumed = MAX (self->audio_pushed_frames - (gint64) queued, 0);
  const gint64 audio_pos = self->audio_base_position_us
                           + consumed * G_GINT64_CONSTANT (1000000) / self->stream_info.sample_rate;

  return apply_drift_correction (raw, MAX (audio_pos, 0));
}

static gint64
next_deadline (OePlaybackSession *self, gint64 now_us)
{
  const gint64 interval = frame_interval_us (self);
  const gint64 elapsed = now_us - self->anchor_time_us;
  const gint64 steps = elapsed / interval + 1; /* strictly in the future */

  return self->anchor_time_us + steps * interval;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

OePlaybackSession *
oe_playback_session_new (const OeProject *project)
{
  g_return_val_if_fail (project != NULL, NULL);

  OePlaybackSession *self = g_new0 (OePlaybackSession, 1);
  self->project = project;
  self->state = OE_PLAYBACK_STOPPED;
  self->last_position_us = 0;
  self->anchor_time_us = g_get_monotonic_time ();

  refresh_sequence (self);

  self->worker = oe_media_playback_worker_new (on_worker_audio, self);
  return self;
}

void
oe_playback_session_free (OePlaybackSession *session)
{
  if (session == NULL)
    return;

  halt_streaming (session);
  g_clear_pointer (&session->stream, oe_audio_output_close_stream);
  g_clear_pointer (&session->worker, oe_media_playback_worker_free);
  close_video_decoder (session);

  if (session->sequence_valid)
    oe_sequence_clear (&session->sequence);

  g_clear_pointer (&session->audio_failed_path, g_free);
  g_free (session);
}

void
oe_playback_session_set_observer (OePlaybackSession *session, OePlaybackNotifyFunc notify,
                                  gpointer user_data)
{
  g_return_if_fail (session != NULL);

  session->notify = notify;
  session->notify_data = user_data;
}

void
oe_playback_session_set_frame_func (OePlaybackSession *session, OePlaybackFrameFunc frame_func,
                                    gpointer user_data)
{
  g_return_if_fail (session != NULL);

  session->frame_func = frame_func;
  session->frame_data = user_data;
}

void
oe_playback_session_set_event_func (OePlaybackSession *session, OePlaybackEventFunc event_func,
                                    gpointer user_data)
{
  g_return_if_fail (session != NULL);

  session->event_func = event_func;
  session->event_data = user_data;
}

gboolean
oe_playback_session_play (OePlaybackSession *session, GError **error)
{
  g_return_val_if_fail (session != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  refresh_sequence (session);

  if (session->sequence_end_us <= 0)
    {
      fire_event (session, OE_PLAYBACK_EVENT_NOTHING_TO_PLAY, NULL);
      return TRUE;
    }

  if (session->state == OE_PLAYBACK_PLAYING)
    return TRUE;

  if (session->state == OE_PLAYBACK_PAUSED)
    {
      /* Resume: drift accounting resets — a fresh anchor at the parked
       * position and a fresh audio request (the queue was flushed at
       * pause). */
      restart_streaming (session, session->last_position_us);
      session->state = OE_PLAYBACK_PLAYING;
      fire_notify (session, session->last_position_us);
      return TRUE;
    }

  ensure_stream (session);
  session->video_missing_reported = FALSE;
  g_clear_pointer (&session->video_failed_path, g_free);
  restart_streaming (session, session->last_position_us);
  session->state = OE_PLAYBACK_PLAYING;
  fire_notify (session, session->last_position_us);
  return TRUE;
}

void
oe_playback_session_pause (OePlaybackSession *session)
{
  g_return_if_fail (session != NULL);

  if (session->state != OE_PLAYBACK_PLAYING)
    return;

  session->last_position_us = compute_position (session, g_get_monotonic_time ());
  halt_streaming (session);
  session->state = OE_PLAYBACK_PAUSED;
  fire_notify (session, session->last_position_us);
}

void
oe_playback_session_stop (OePlaybackSession *session)
{
  g_return_if_fail (session != NULL);

  if (session->state == OE_PLAYBACK_STOPPED)
    return;

  if (session->state == OE_PLAYBACK_PLAYING)
    session->last_position_us = compute_position (session, g_get_monotonic_time ());

  halt_streaming (session);
  session->state = OE_PLAYBACK_STOPPED;
  fire_notify (session, session->last_position_us);
}

void
oe_playback_session_seek (OePlaybackSession *session, gint64 position_us)
{
  g_return_if_fail (session != NULL);

  refresh_sequence (session);

  const gint64 clamped = CLAMP (position_us, 0, MAX (session->sequence_end_us, 0));
  session->last_position_us = clamped;

  if (session->state == OE_PLAYBACK_PLAYING)
    {
      /* Clock reset + decoder flush + queue flush, then fresh requests:
       * the full seek discipline. */
      restart_streaming (session, clamped);
    }

  fire_notify (session, clamped);
}

gint64
oe_playback_session_tick (OePlaybackSession *session)
{
  g_return_val_if_fail (session != NULL, g_get_monotonic_time ());

  const gint64 now = g_get_monotonic_time ();

  if (session->state != OE_PLAYBACK_PLAYING)
    return now;

  const gint64 position = compute_position (session, now);

  if (position >= session->sequence_end_us)
    {
      /* End of sequence: stop with the playhead parked at the end. */
      halt_streaming (session);
      session->state = OE_PLAYBACK_STOPPED;
      session->last_position_us = session->sequence_end_us;
      fire_event (session, OE_PLAYBACK_EVENT_END_OF_SEQUENCE, NULL);
      fire_notify (session, session->sequence_end_us);
      return now;
    }

  session->last_position_us = position;
  feed_audio (session, position);
  update_video (session, position);
  fire_notify (session, position);

  return next_deadline (session, now);
}

gint64
oe_playback_session_get_position (const OePlaybackSession *session)
{
  g_return_val_if_fail (session != NULL, 0);

  if (session->state != OE_PLAYBACK_PLAYING)
    return session->last_position_us;

  return compute_position ((OePlaybackSession *) session, g_get_monotonic_time ());
}

OePlaybackState
oe_playback_session_get_state (const OePlaybackSession *session)
{
  g_return_val_if_fail (session != NULL, OE_PLAYBACK_STOPPED);

  return session->state;
}

gint64
oe_playback_session_get_sequence_end (const OePlaybackSession *session)
{
  g_return_val_if_fail (session != NULL, 0);

  return session->sequence_end_us;
}
