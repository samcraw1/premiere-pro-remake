
/* oe_playback_session.c — the GTK-free playback clock (Phase 5).
 *
 * Time discipline: integer µs end to end. The position while playing is
 * always anchor_position + (monotonic_now - anchor_time); play, resume,
 * and seek RE-ANCHOR (fresh pair) rather than accumulating deltas, which
 * is the pause/resume drift accounting. On a real audio device the
 * consumed-audio position corrects the ANCHOR (slewed, with a snap
 * bound) so the wall clock converges on the device clock instead of
 * fighting it; on the dummy driver or with no device the wall clock
 * alone paces playback.
 *
 * Threading: every entry point runs on the main thread. The media
 * worker's chunk deliveries and the render worker's frames are invoked
 * onto the main context, so no lock is taken anywhere in this file.
 *
 * Audio mixing (Phase 10 Wave B): the session is a multi-track mixer —
 * each mix window sums EVERY audible audio track's decoded contribution
 * through the shared factor chain into one interleaved buffer and queues
 * the final regions; a metering tap observes per-span peaks on the way
 * through. The queue is CONTIGUOUS in sequence time: every window starts
 * exactly where the previous one ended (the first at the playhead), and
 * spans with no audible clip queue silence, so the frames the device
 * consumes always map back to a sequence position. GTK never appears
 * here.
 *
 * Video: frames render on the render worker's thread (oe_render_worker)
 * and arrive on the main context; the tick only asks. A slow decode can
 * therefore never stall the transport or the audio feed.
 */

#include "oe_playback_session.h"

#include "../core/oe_audio_buffer.h"
#include "../core/oe_audio_factor.h"
#include "../core/oe_fades.h"

#include "../core/oe_time.h"
#include "../media/oe_render_worker.h"
#include "../playback/oe_audio_output.h"
#include "oe_log.h"

/* Audio feed policy: keep the queue filled through position + lookahead,
 * one bounded request at a time. Windows start where the previous one
 * ended, so the lookahead is real buffering — audio the device holds
 * beyond the playhead — not a content offset. */
#define AUDIO_LOOKAHEAD_US G_GINT64_CONSTANT (300000)
#define AUDIO_REQUEST_SPAN_US G_GINT64_CONSTANT (1000000)
/* The dummy driver never consumes: cap what we push into the void. */
#define AUDIO_DUMMY_QUEUE_CAP_FRAMES (48000 * 2) /* ~2 s at 48 kHz stereo */
/* Windows submitted per tick at most: a run of sub-frame slivers at
 * clip edges must not spin the feed. */
#define AUDIO_FEED_ROUNDS_PER_TICK 8

/* Drift correction: slew the anchor up to 1 ms per tick toward the
 * consumed-audio position; re-anchor outright when the gap exceeds the
 * snap bound (device drop or stall). The bound is at least one frame
 * and at least two device periods: the reported queue depth moves in
 * whole periods, so a smaller bound would snap on every callback. */
#define DRIFT_SLEW_US 1000
#define DRIFT_SNAP_US G_GINT64_CONSTANT (40000)
/* After a (re)start the position holds at the anchor until the device
 * has consumed its first period, so video waits for audio instead of
 * racing ahead and snapping back; a device that never starts releases
 * the hold after this long and the wall clock paces alone. */
#define AUDIO_START_GRACE_US G_GINT64_CONSTANT (500000)

/* One contributing (clip, track) pair of the active mix window,
 * captured at window-open time so deliveries never re-read the model. */
typedef struct
{
  guint media_ref;         /* resolved to a path at request time */
  gint64 clip_position_us; /* clip placement in sequence time */
  gint64 clip_source_in_us;
  gint64 clip_end_seq_us; /* sequence time where the clip ends */
  guint64 fade_in_us;     /* the clip's shared envelope (Wave B) */
  guint64 fade_out_us;
  gint64 src_from_us; /* the decoded source range */
  gint64 src_to_us;
  gint32 factor[2]; /* buffer-constant chain factors (D1/D5) */
} MixSlot;

/* The active mix window (D1): the span [seq_start_us, seq_start_us +
 * window_frames) is filled by decoding each contributing audio track's
 * range SEQUENTIALLY (the worker owns one request at a time) and
 * summing into one interleaved buffer in track-array order (higher
 * track indexes mix above, per the documented layering order). The
 * buffer is session-owned; the final contributor's writes complete
 * regions that are pushed progressively to the device queue. */
typedef struct
{
  gboolean active;     /* a window is being assembled */
  gint64 seq_start_us; /* sequence time of the window's first frame */
  gsize window_frames; /* window length in frames at the device rate */
  gfloat *mix;         /* window_frames * channels, session-owned */
  gsize pushed_to;     /* frames already queued (final region) */
  MixSlot *slots;      /* contributing tracks in track-array order */
  gsize n_slots;
  gsize next_slot;    /* next slot to submit */
  gsize current_slot; /* slot whose request is in flight */
} MixWindow;

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

  /* Injectable clock: NULL keeps the production default. Tests install a
   * virtual clock so wall-cadence assertions never need real sleeps. */
  OePlaybackTimeSourceFunc time_func;
  gpointer time_data;

  OeMediaPlaybackWorker *worker;
  guint audio_generation; /* bumped on every stream restart */

  OeAudioStream *stream;
  OeAudioDeviceInfo stream_info;
  gint64 audio_base_position_us;  /* sequence position when the queue was flushed */
  gint64 audio_pushed_frames;     /* frames accepted since the flush */
  gint64 audio_pushed_through_us; /* sequence time of the end of pushed coverage —
                                     pushes begin LOOKAHEAD ahead of the playhead,
                                     so this is NOT base + frames */
  gboolean audio_outstanding;     /* a mix window is being assembled */
  MixWindow mix;
  gchar *audio_failed_path; /* media that failed this generation */

  /* Main-context observers, GTK-free: per-push peak levels (D6) and the
   * deterministic mixed-audio seam tests capture through. */
  OePlaybackMeterFunc meter_func;
  gpointer meter_data;
  OePlaybackMixFunc mix_func;
  gpointer mix_data;

  OeRenderWorker *render_worker;   /* owns the seam decoder cache off-thread */
  guint video_generation;          /* bumped on every snapshot refresh */
  gint64 video_requested_index;    /* sequence frame index last asked for */
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

/* The single "now" reader: production defaults to g_get_monotonic_time();
 * tests install a virtual clock through oe_playback_session_set_time_source(). */
static gint64
session_now (const OePlaybackSession *self)
{
  if (self->time_func != NULL)
    return self->time_func (self->time_data);
  return g_get_monotonic_time ();
}

/* The snap bound for this stream: the larger of the fixed floor and
 * two device periods (the queue-depth step size). */
static gint64
drift_snap_us (const OePlaybackSession *self)
{
  gint64 period_us = 0;

  if (self->stream_info.sample_rate > 0 && self->stream_info.buffer_frames > 0)
    period_us = (gint64) self->stream_info.buffer_frames * G_GINT64_CONSTANT (1000000)
                / self->stream_info.sample_rate;

  return MAX (DRIFT_SNAP_US, 2 * period_us);
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

/* Render seam: frames come from the render worker, which owns the
 * shared compositor (per-source decoder cache + layered blend) on its
 * own thread against its own copy of the sequence snapshot. */
static void on_render_frame (OeRenderWorkerFrame *frame, const GError *error, gpointer user_data);

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

  /* The render worker gets its own deep copy plus every media path the
   * snapshot can reference, resolved HERE: the project is main-thread
   * state and the worker must never read it. Frames still in flight
   * from the outgoing snapshot are dropped by the generation guard. */
  OeSequence *copy = g_new0 (OeSequence, 1);

  oe_sequence_copy (copy, &self->sequence);

  GHashTable *paths = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);

  if (self->sequence.tracks != NULL)
    {
      for (guint t = 0; t < self->sequence.tracks->len; t++)
        {
          const OeTrack *track = g_ptr_array_index (self->sequence.tracks, t);

          if (track->clips == NULL)
            continue;

          for (guint c = 0; c < track->clips->len; c++)
            {
              const OeClip *clip = g_ptr_array_index (track->clips, c);

              if (clip->kind != OE_CLIP_MEDIA
                  || g_hash_table_contains (paths, GUINT_TO_POINTER (clip->media_ref)))
                continue;

              gchar *path
                  = oe_project_dup_media_path ((OeProject *) self->project, clip->media_ref);

              if (path != NULL)
                g_hash_table_insert (paths, GUINT_TO_POINTER (clip->media_ref), path);
            }
        }
    }

  self->video_generation++;
  self->video_requested_index = -1;

  if (self->render_worker != NULL)
    oe_render_worker_set_sequence (self->render_worker, copy, paths);
  else
    {
      oe_sequence_clear (copy);
      g_free (copy);
      g_hash_table_unref (paths);
    }
}

/* ------------------------------------------------------------------ */
/* Audio: multi-track mix window, stream lifecycle, delivery           */
/* ------------------------------------------------------------------ */

static void
mix_window_reset (OePlaybackSession *self)
{
  self->mix.active = FALSE;
  g_clear_pointer (&self->mix.mix, g_free);
  g_clear_pointer (&self->mix.slots, g_free);
  self->mix.n_slots = 0;
  self->mix.next_slot = 0;
  self->mix.current_slot = 0;
  self->mix.window_frames = 0;
  self->mix.pushed_to = 0;
}

/* Queue the window's final region [pushed_to, to_frames): every
 * contributing track has written everything it has, so the region is
 * final mixed audio. Applies the shared hard clamp — the same
 * "clamp once, at the last word" rule the export mixdown uses, with the
 * device queue as the playback side's last word — and fires the
 * metering and deterministic-delivery observers per pushed span. */
static void
mix_window_push (OePlaybackSession *self, gsize to_frames)
{
  if (self->stream == NULL || to_frames <= self->mix.pushed_to)
    return;

  const gsize from = self->mix.pushed_to;
  const gsize count = to_frames - from;
  const int channels = MAX (self->stream_info.channels, 1);
  gfloat *span = self->mix.mix + from * (gsize) channels;

  for (gsize i = 0; i < count * (gsize) channels; i++)
    span[i] = CLAMP (span[i], -1.0f, 1.0f);

  const gsize pushed = oe_audio_output_queue (self->stream, span, count);

  self->audio_pushed_frames += (gint64) pushed;

  if (pushed > 0)
    self->audio_pushed_through_us
        = self->mix.seq_start_us
          + (gint64) (from + pushed) * G_GINT64_CONSTANT (1000000) / self->stream_info.sample_rate;

  if (pushed == 0)
    return;

  if (self->meter_func != NULL)
    {
      /* Per-channel peak of the pushed span (D6): the span IS the
       * per-chunk mix unit, delivered on the main context — no locks,
       * no worker access. Peaks stop arriving when chunks stop; the
       * display decays toward silence on its own. */
      gfloat *peaks = g_newa (gfloat, (gsize) channels);

      for (int ch = 0; ch < channels; ch++)
        peaks[ch] = oe_audio_buffer_peak (span, pushed, channels, ch);

      self->meter_func (self, peaks, channels, self->meter_data);
    }

  if (self->mix_func != NULL)
    self->mix_func (self,
                    self->mix.seq_start_us
                        + (gint64) from * G_GINT64_CONSTANT (1000000)
                              / self->stream_info.sample_rate,
                    pushed, channels, self->stream_info.sample_rate, span, self->mix_data);

  self->mix.pushed_to = from + pushed;
}

/* Submit the next contributing track's decode, or close the window when
 * the last one is done. A track whose media failed this generation
 * contributes silence — skipped here exactly like the export mixdown
 * skips a failing file. */
static void
mix_window_advance (OePlaybackSession *self)
{
  while (self->mix.active)
    {
      if (self->mix.next_slot >= self->mix.n_slots)
        {
          /* Every contributing track decoded: the tail beyond the last
           * progressive push is final — queue it, close the window. */
          mix_window_push (self, self->mix.window_frames);
          mix_window_reset (self);
          self->audio_outstanding = FALSE;
          return;
        }

      const gsize index = self->mix.next_slot;
      const MixSlot *slot = &self->mix.slots[index];
      gchar *path = oe_project_dup_media_path ((OeProject *) self->project, slot->media_ref);

      if (path == NULL || g_strcmp0 (path, self->audio_failed_path) == 0)
        {
          g_free (path);
          self->mix.next_slot++;
          continue;
        }

      self->mix.current_slot = index;
      self->mix.next_slot++;

      oe_log (OE_LOG_LEVEL_DEBUG, "audio request: '%s' [%lld, %lld) gen %u (mix slot %zu/%zu)",
              path, (long long) slot->src_from_us, (long long) slot->src_to_us,
              self->audio_generation, index + 1, self->mix.n_slots);
      oe_media_playback_worker_request (self->worker, path, slot->src_from_us, slot->src_to_us,
                                        self->stream_info.sample_rate, self->stream_info.channels,
                                        self->audio_generation);
      g_free (path);
      return; /* request in flight; its exhaustion advances the window */
    }
}

/* The earliest audio clip edge (a start or an end) strictly after
 * @from_us on ANY audio track, capped at @limit_us. Windows end at the
 * next edge so the set of covering clips is constant across a window:
 * a clip that begins mid-span gets its own window instead of losing
 * its head to the previous one, and a clip that ends mid-span is not
 * padded with the next one's audio. */
static gint64
next_audio_edge (const OeSequence *seq, gint64 from_us, gint64 limit_us)
{
  gint64 edge = limit_us;

  for (guint t = 0; t < seq->tracks->len; t++)
    {
      const OeTrack *track = g_ptr_array_index (seq->tracks, t);

      if (track->kind != OE_TRACK_AUDIO || track->clips == NULL)
        continue;

      for (guint c = 0; c < track->clips->len; c++)
        {
          const OeClip *clip = g_ptr_array_index (track->clips, c);
          const gint64 start = clip->position_us;
          const gint64 stop = start + (clip->source_out_us - clip->source_in_us);

          if (start > from_us && start < edge)
            edge = start;
          if (stop > from_us && stop < edge)
            edge = stop;
          if (start >= edge)
            break; /* position-ordered: nothing later can lower the edge */
        }
    }

  return edge;
}

/* Open the next mix window: it starts exactly where pushed coverage
 * ends (the playhead on a fresh start) and runs to the next clip edge
 * or the request span, whichever comes first. Every audible audio
 * track's covering clip contributes a slot, in track-array order; gaps
 * and inaudible tracks contribute silence (no slot, zero in the shared
 * buffer). A span with NO contributing track is a zero-slot window:
 * its silence is queued at once, so the device queue stays contiguous
 * in sequence time and its consumed depth keeps mapping to a position. */
static void
submit_audio_request (OePlaybackSession *self)
{
  if (self->stream == NULL || self->worker == NULL)
    return;

  const OeSequence *seq = &self->sequence;

  if (seq->tracks == NULL)
    return;

  const gint64 seq_from = self->audio_pushed_through_us;
  const gint64 seq_to = next_audio_edge (seq, seq_from, seq_from + AUDIO_REQUEST_SPAN_US);
  const gsize window_frames = (gsize) ((seq_to - seq_from) * (gint64) self->stream_info.sample_rate
                                       / G_GINT64_CONSTANT (1000000));

  if (window_frames == 0)
    {
      /* Sub-frame sliver between two edges: nothing to queue, step over
       * it so the feed makes progress. */
      self->audio_pushed_through_us = seq_to;
      return;
    }

  /* D5 audibility: one any-solo scan over the audio tracks, then the
   * per-track mute/solo decision — the same rule the export mixdown
   * applies. A silenced track is skipped before its media is even
   * opened. */
  gboolean any_solo = FALSE;

  for (guint t = 0; t < seq->tracks->len && !any_solo; t++)
    {
      const OeTrack *track = g_ptr_array_index (seq->tracks, t);

      if (track->kind == OE_TRACK_AUDIO && track->clips != NULL && track->audio.solo != 0)
        any_solo = TRUE;
    }

  const int channels = MAX (self->stream_info.channels, 1);
  MixSlot *slots = g_new0 (MixSlot, seq->tracks->len);
  gsize n_slots = 0;

  for (guint t = 0; t < seq->tracks->len; t++)
    {
      const OeTrack *track = g_ptr_array_index (seq->tracks, t);

      if (track->kind != OE_TRACK_AUDIO || track->clips == NULL)
        continue;

      /* Per-track clip covering the window start — clips are
       * position-ordered, so the first cover wins. */
      const OeClip *clip = NULL;

      for (guint c = 0; c < track->clips->len; c++)
        {
          const OeClip *cand = g_ptr_array_index (track->clips, c);
          const gint64 length = cand->source_out_us - cand->source_in_us;

          if (seq_from >= cand->position_us && seq_from < cand->position_us + length)
            {
              clip = cand;
              break;
            }
        }

      if (clip == NULL)
        continue;

      if (!oe_audio_audible (track->audio.mute, track->audio.solo, any_solo))
        continue;

      const gint64 src_from = CLAMP (clip->source_in_us + (seq_from - clip->position_us),
                                     clip->source_in_us, clip->source_out_us);
      const gint64 src_to = MIN (clip->source_out_us, src_from + (seq_to - seq_from));

      if (src_to <= src_from)
        continue;

      MixSlot *slot = &slots[n_slots];

      slot->media_ref = clip->media_ref;
      slot->clip_position_us = clip->position_us;
      slot->clip_source_in_us = clip->source_in_us;
      slot->clip_end_seq_us = clip->position_us + (clip->source_out_us - clip->source_in_us);
      slot->fade_in_us = clip->visual.fade_in_us;
      slot->fade_out_us = clip->visual.fade_out_us;
      slot->src_from_us = src_from;
      slot->src_to_us = src_to;

      /* Buffer-constant chain factors (D1): the non-fade stages of the
       * shared chain resolve ONCE per (clip, track, channel) per window;
       * the fade envelope keeps its per-sample cadence at delivery. */
      oe_audio_factor (OE_AUDIO_UNITY, clip->audio.gain, clip->audio.pan, track->audio.volume,
                       track->audio.pan, 1, slot->factor);
      n_slots++;
    }

  self->mix.active = TRUE;
  self->mix.seq_start_us = seq_from;
  self->mix.window_frames = window_frames;
  self->mix.pushed_to = 0;
  self->mix.next_slot = 0;
  self->mix.current_slot = 0;
  self->mix.n_slots = n_slots;
  self->mix.slots = slots;
  self->mix.mix = g_malloc0 (window_frames * (gsize) channels * sizeof (gfloat));
  self->audio_outstanding = TRUE;

  /* With slots this submits the first decode; with none it queues the
   * silent window and closes it on the spot. */
  mix_window_advance (self);
}

/* Keep the queue filled through position + lookahead. Works unchanged on
 * the dummy driver: pushed-coverage advances with the wall clock, and the
 * dummy cap bounds how much is pushed into the void. */
static void
feed_audio (OePlaybackSession *self, gint64 position_us)
{
  if (self->stream == NULL || self->worker == NULL)
    return;

  if (self->stream_info.sample_rate <= 0)
    return;

  /* Coverage fell behind the playhead: the queue ran dry and the device
   * played silence past the end of what we pushed (a stalled decode, a
   * suspended process). Resume contiguous from the playhead — nothing
   * useful is left to preserve — with the consumed-audio accounting
   * rebased so the clock still maps frames to positions. */
  if (!self->stream_info.is_dummy && !self->audio_outstanding
      && self->audio_pushed_through_us < position_us)
    {
      oe_audio_output_flush (self->stream);
      self->audio_base_position_us = position_us;
      self->audio_pushed_frames = 0;
      self->audio_pushed_through_us = position_us;
    }

  for (int round = 0; round < AUDIO_FEED_ROUNDS_PER_TICK; round++)
    {
      if (self->audio_outstanding)
        return;

      if (self->audio_pushed_through_us >= position_us + AUDIO_LOOKAHEAD_US)
        return;

      if (self->stream_info.is_dummy
          && oe_audio_output_queued_frames (self->stream) > AUDIO_DUMMY_QUEUE_CAP_FRAMES)
        return;

      const gint64 before = self->audio_pushed_through_us;

      submit_audio_request (self);

      if (!self->audio_outstanding && self->audio_pushed_through_us == before)
        return; /* no progress (push rejected): retry next tick */
    }
}

static void
on_worker_audio (OePlaybackAudioChunk *chunk, const GError *error, guint generation,
                 gpointer user_data)
{
  OePlaybackSession *self = user_data;

  if (generation != self->audio_generation)
    return; /* stale delivery — the worker's trampoline owns it. NULL-chunk
               signals carry the owning request's generation too, so the
               multi-source window never advances on a stale end-of-range
               from a superseded decode. */

  if (chunk == NULL && error == NULL)
    {
      /* Range exhausted: submit the next contributing track's decode, or
       * close the window when the last one is done. */
      if (self->mix.active)
        mix_window_advance (self);
      else
        self->audio_outstanding = FALSE;
      return;
    }

  if (chunk == NULL)
    {
      /* Decode failed: report once per generation, continue without —
       * the failed slot contributes silence, the rest of the window
       * still assembles. */
      if (self->audio_failed_path == NULL && error->message != NULL)
        {
          self->audio_failed_path = g_strdup (error->message);
          fire_event (self, OE_PLAYBACK_EVENT_MISSING_MEDIA_SKIPPED, error->message);
        }
      if (self->mix.active)
        mix_window_advance (self);
      else
        self->audio_outstanding = FALSE;
      return;
    }

  if (!self->mix.active || self->mix.current_slot >= self->mix.n_slots)
    return; /* late chunk with no window — the trampoline owns the chunk */

  /* Map the chunk's source time through the owning slot captured at
   * window-open time (source in/out is the only mapping the model
   * defines), then write into the shared mix buffer. */
  const MixSlot *slot = &self->mix.slots[self->mix.current_slot];
  const int channels = MAX (self->stream_info.channels, 1);
  const gint64 seq_start = slot->clip_position_us + (chunk->source_us - slot->clip_source_in_us);
  gint64 offset = 0;

  if (chunk->sample_rate > 0)
    offset = (seq_start - self->mix.seq_start_us) * (gint64) chunk->sample_rate
             / G_GINT64_CONSTANT (1000000);

  gsize drop = 0;

  if (offset < 0)
    {
      drop = (gsize) (-offset); /* sub-frame floor: whole frames, skipped */
      offset = 0;
    }

  if (drop >= chunk->n_frames)
    return;

  gsize n = chunk->n_frames - drop;

  if (offset + n > self->mix.window_frames)
    n = self->mix.window_frames - offset; /* clip end / window bound */

  if (n == 0)
    return;

  /* Shared factor chain (D1) applied buffer-constant per channel; the
   * fade envelope keeps its per-sample cadence and is skipped entirely
   * for unfaded clips. Channels beyond stereo downmix to the pan pair's
   * mean, which preserves the equal-sum law (unity at center pan). */
  const gfloat f_left = (gfloat) slot->factor[0] / (gfloat) OE_AUDIO_UNITY;
  const gfloat f_right = (gfloat) slot->factor[1] / (gfloat) OE_AUDIO_UNITY;
  const gfloat f_rest = (f_left + f_right) / 2.0f;
  const gboolean faded = slot->fade_in_us > 0 || slot->fade_out_us > 0;
  gfloat *dst = self->mix.mix + (gsize) offset * (gsize) channels;
  const gfloat *src = chunk->interleaved + drop * (gsize) channels;

  for (gsize i = 0; i < n; i++)
    {
      gfloat g = 1.0f;

      if (faded)
        {
          const gint64 t_us
              = seq_start
                + (gint64) (drop + i) * G_GINT64_CONSTANT (1000000) / (gint64) chunk->sample_rate;

          g = (gfloat) oe_fade_gain (t_us, slot->clip_position_us, slot->clip_end_seq_us,
                                     slot->fade_in_us, slot->fade_out_us)
              / (gfloat) OE_FADE_SCALE;
        }

      for (int ch = 0; ch < channels; ch++)
        {
          const gfloat f = ch == 0 ? f_left : (ch == 1 ? f_right : f_rest);

          dst[i * (gsize) channels + ch] += src[i * (gsize) channels + ch] * g * f;
        }
    }

  /* The final contributor's writes complete regions: push them
   * progressively (single-track playback pushes per chunk, exactly the
   * pre-Wave-B cadence). */
  if (self->mix.current_slot == self->mix.n_slots - 1)
    mix_window_push (self, (gsize) offset + n);

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

  /* The in-flight window's memory dies with the generation: stale
   * deliveries are dropped by the generation guard before any write. */
  mix_window_reset (self);

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
  self->audio_pushed_through_us = position_us; /* nothing queued yet */
  self->anchor_position_us = position_us;
  self->anchor_time_us = session_now (self);

  feed_audio (self, position_us);
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
/* Video: shared-seam frames, rendered off-thread                      */
/* ------------------------------------------------------------------ */

#define VIDEO_DECODE_BOX_W 1280
#define VIDEO_DECODE_BOX_H 720

/* Ask the render worker for the frame at @position, or clear the
 * monitor when no video is scheduled. The frame arrives later on the
 * main context (on_render_frame); the tick never waits for it. With
 * @force the same-frame dedup is bypassed — the paused repaint and the
 * paused seek need a fresh render even when the frame index did not
 * move. */
static void
update_video_full (OePlaybackSession *self, gint64 position_us, gboolean force)
{
  OePlaybackMapping map;
  const gboolean active
      = oe_playback_session_map (&self->sequence, OE_TRACK_VIDEO, position_us, &map);

  if (!active)
    {
      if (self->video_active)
        {
          self->video_active = FALSE;
          self->video_requested_index = -1;
          if (self->frame_func != NULL)
            self->frame_func (self, NULL, self->frame_data);
        }
      return;
    }

  const gint64 frame_index = oe_time_us_to_frame (position_us, self->sequence.frame_rate);

  if (!force && self->video_active && frame_index == self->video_requested_index)
    return; /* same frame — stills render once per source frame */

  if (self->render_worker == NULL)
    return;

  self->video_active = TRUE;
  self->video_requested_index = frame_index;
  oe_render_worker_request (self->render_worker, position_us, frame_index, self->video_generation,
                            force, VIDEO_DECODE_BOX_W, VIDEO_DECODE_BOX_H);
}

static void
update_video (OePlaybackSession *self, gint64 position_us)
{
  update_video_full (self, position_us, FALSE);
}

/* Render worker → session, on the main context. Frames from a
 * superseded snapshot or run (generation mismatch) are dropped: a seek
 * or an edit has moved on and the monitor must not flash a stale
 * picture. Failures report once per run and the transport continues. */
static void
on_render_frame (OeRenderWorkerFrame *frame, const GError *error, gpointer user_data)
{
  OePlaybackSession *self = user_data;

  if (frame->generation != self->video_generation)
    return;

  if (frame->rgba == NULL)
    {
      if (!self->video_missing_reported)
        {
          self->video_missing_reported = TRUE;
          fire_event (self, OE_PLAYBACK_EVENT_MISSING_MEDIA_SKIPPED,
                      error != NULL ? error->message : "cannot render the sequence");
        }
      return;
    }

  if (!self->video_active)
    return; /* the position left every video clip while this rendered */

  OePlaybackVideoFrame *out = g_new0 (OePlaybackVideoFrame, 1);
  OePlaybackMapping map;

  oe_playback_session_map (&self->sequence, OE_TRACK_VIDEO, frame->t_us, &map);
  out->source_us = map.source_us;
  out->width = frame->width;
  out->height = frame->height;
  out->rgba = frame->rgba;
  frame->rgba = NULL; /* ownership moves to the delivered frame */

  if (self->frame_func != NULL)
    self->frame_func (self, out, self->frame_data); /* ownership transfers */
  else
    oe_playback_video_frame_free (out);
}

void
oe_playback_session_repaint_paused (OePlaybackSession *session)
{
  g_return_if_fail (session != NULL);

  if (session->state == OE_PLAYBACK_PLAYING)
    return; /* the tick owns the frame while playing */

  /* A visual edit mutated the live project since the last play/seek:
   * re-snapshot before rendering, or the repaint serves the frozen
   * pre-edit sequence and the monitor never shows the edit. */
  refresh_sequence (session);
  session->video_missing_reported = FALSE;

  update_video_full (session, session->last_position_us, TRUE);
}

/* ------------------------------------------------------------------ */
/* Clock                                                               */
/* ------------------------------------------------------------------ */

/* The anchor-based position: pure, no device access. */
static gint64
compute_position (const OePlaybackSession *self, gint64 now_us)
{
  const gint64 raw = self->anchor_position_us + (now_us - self->anchor_time_us);

  return MAX (raw, 0);
}

/* Drift correction against the device (real streams only — the dummy
 * driver never consumes, so its queue depth is not a pacing signal).
 * The sequence position of the audio the device has consumed is
 * base + consumed / rate, exact because the queue is contiguous from
 * base. The ANCHOR is corrected — slewed by up to DRIFT_SLEW_US per
 * tick, re-anchored outright past the snap bound — so the correction
 * converges instead of choosing between two clocks every tick. Half a
 * device period is added because the reported depth lags true playback
 * by up to one period (it moves in whole callbacks). Nothing happens
 * until the device has consumed anything: the clock waits for audio to
 * start rather than racing ahead of it. */
static void
sync_clock_to_audio (OePlaybackSession *self, gint64 now_us)
{
  if (self->stream == NULL || self->stream_info.is_dummy || self->stream_info.sample_rate <= 0)
    return;

  const gsize queued = oe_audio_output_queued_frames (self->stream);
  const gint64 consumed = self->audio_pushed_frames - (gint64) queued;

  if (consumed <= 0)
    {
      /* Audio not yet flowing (still decoding, or queued but before the
       * device's first callback): hold the position here — a fresh time
       * anchor each tick — for the grace period. */
      if (now_us - self->anchor_time_us < AUDIO_START_GRACE_US)
        self->anchor_time_us = now_us;
      return;
    }

  const gint64 phase = (gint64) self->stream_info.buffer_frames / 2;
  const gint64 audio_pos
      = self->audio_base_position_us
        + (consumed + phase) * G_GINT64_CONSTANT (1000000) / self->stream_info.sample_rate;
  const gint64 wall_pos = compute_position (self, now_us);
  const gint64 diff = audio_pos - wall_pos;
  const gint64 snap = drift_snap_us (self);

  if (diff > snap || diff < -snap)
    {
      self->anchor_position_us = MAX (audio_pos, 0);
      self->anchor_time_us = now_us;
    }
  else if (diff > DRIFT_SLEW_US)
    self->anchor_position_us += DRIFT_SLEW_US;
  else if (diff < -DRIFT_SLEW_US)
    self->anchor_position_us -= DRIFT_SLEW_US;
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
  self->anchor_time_us = session_now (self);

  self->render_worker = oe_render_worker_new (on_render_frame, self);
  self->worker = oe_media_playback_worker_new (on_worker_audio, self);
  self->video_requested_index = -1;

  refresh_sequence (self);
  return self;
}

void
oe_playback_session_free (OePlaybackSession *session)
{
  if (session == NULL)
    return;

  halt_streaming (session);
  session->video_generation++; /* frames still in flight are dropped on delivery */
  g_clear_pointer (&session->render_worker, oe_render_worker_free);
  g_clear_pointer (&session->stream, oe_audio_output_close_stream);
  g_clear_pointer (&session->worker, oe_media_playback_worker_free);

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

void
oe_playback_session_set_time_source (OePlaybackSession *session, OePlaybackTimeSourceFunc time_func,
                                     gpointer user_data)
{
  g_return_if_fail (session != NULL);

  session->time_func = time_func;
  session->time_data = user_data;
}

void
oe_playback_session_set_meter_func (OePlaybackSession *session, OePlaybackMeterFunc func,
                                    gpointer user_data)
{
  g_return_if_fail (session != NULL);

  session->meter_func = func;
  session->meter_data = user_data;
}

void
oe_playback_session_set_mix_func (OePlaybackSession *session, OePlaybackMixFunc func,
                                  gpointer user_data)
{
  g_return_if_fail (session != NULL);

  session->mix_func = func;
  session->mix_data = user_data;
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

  session->last_position_us = compute_position (session, session_now (session));
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
    session->last_position_us = compute_position (session, session_now (session));

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
  else
    {
      /* Scrubbing: the monitor follows the playhead. Forced, because the
       * frame index may not have moved but the snapshot did. */
      session->video_missing_reported = FALSE;
      update_video_full (session, clamped, TRUE);
    }

  fire_notify (session, clamped);
}

gint64
oe_playback_session_tick (OePlaybackSession *session)
{
  g_return_val_if_fail (session != NULL, g_get_monotonic_time ());

  const gint64 now = session_now (session);

  if (session->state != OE_PLAYBACK_PLAYING)
    return now;

  sync_clock_to_audio (session, now);

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

  return compute_position (session, session_now (session));
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
