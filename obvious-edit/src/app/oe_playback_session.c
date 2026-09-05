
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
 *
 * Audio mixing (Phase 10 Wave B): the session is a multi-track mixer —
 * each mix window sums EVERY audible audio track's decoded contribution
 * through the shared factor chain into one interleaved buffer and queues
 * the final regions; a metering tap observes per-span peaks on the way
 * through. GTK never appears here.
 */

#include "oe_playback_session.h"

#include "../core/oe_audio_buffer.h"
#include "../core/oe_audio_factor.h"
#include "../core/oe_fades.h"

#include "../core/oe_time.h"
#include "../media/oe_render.h"
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

  OeRenderSource render_source;    /* sequence snapshot + resolver */
  OeRenderSession *render_session; /* shared seam decoder cache, lazy */
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

/* The single "now" reader: production defaults to g_get_monotonic_time();
 * tests install a virtual clock through oe_playback_session_set_time_source(). */
static gint64
session_now (const OePlaybackSession *self)
{
  if (self->time_func != NULL)
    return self->time_func (self->time_data);
  return g_get_monotonic_time ();
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

/* Render seam: the shared compositor (per-source decoder cache +
 * layered blend) rendered lazily against the deep-copied sequence
 * snapshot. The resolver hands the seam the project's media paths. */
static gchar *
resolve_media_path (guint media_ref, gpointer user_data)
{
  OePlaybackSession *self = user_data;

  return oe_project_dup_media_path ((OeProject *) self->project, media_ref);
}

static void
close_render_session (OePlaybackSession *self)
{
  g_clear_pointer (&self->render_session, oe_render_session_free);
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

  /* The decoder cache keys to the outgoing snapshot; a stale session
   * would serve frames from files the new snapshot no longer maps. */
  self->render_source.sequence = &self->sequence;
  self->render_source.resolve_path = resolve_media_path;
  self->render_source.resolve_data = self;
  close_render_session (self);
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

/* Open a mix window covering [position + lookahead, …): collect every
 * audible audio track's clip at the window start, in track-array order,
 * sized by each slot's decoded span. Gaps and inaudible tracks
 * contribute silence (no slot, zero in the shared buffer); with NO
 * contributing track the window stays closed and the feed retries on a
 * later tick. */
static void
submit_audio_request (OePlaybackSession *self, gint64 position_us)
{
  if (self->stream == NULL || self->worker == NULL)
    return;

  const gint64 seq_from = position_us + AUDIO_LOOKAHEAD_US;
  const OeSequence *seq = &self->sequence;

  if (seq->tracks == NULL)
    return;

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
  gsize window_frames = 0;

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
      const gint64 src_to = MIN (clip->source_out_us, src_from + AUDIO_REQUEST_SPAN_US);

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

      window_frames = MAX (window_frames,
                           (gsize) ((src_to - src_from) * (gint64) self->stream_info.sample_rate
                                    / G_GINT64_CONSTANT (1000000)));
      n_slots++;
    }

  if (n_slots == 0 || window_frames == 0)
    {
      g_free (slots);
      return; /* all silence for this span: feed retries on a later tick */
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

  mix_window_advance (self);
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

  if (self->audio_pushed_through_us >= position_us + AUDIO_LOOKAHEAD_US)
    return;

  if (self->stream_info.is_dummy
      && oe_audio_output_queued_frames (self->stream) > AUDIO_DUMMY_QUEUE_CAP_FRAMES)
    return;

  submit_audio_request (self, position_us);
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
/* Video: shared-seam frame delivery                                   */
/* ------------------------------------------------------------------ */

#define VIDEO_DECODE_BOX_W 1280
#define VIDEO_DECODE_BOX_H 720

static OeRenderSession *
ensure_render_session (OePlaybackSession *self)
{
  if (self->render_session == NULL)
    self->render_session = oe_render_session_new (&self->render_source);

  return self->render_session;
}

/* Decode and deliver the frame at @position, or clear the monitor when
 * no video is scheduled. Rendering runs through the shared render seam
 * (decoder cache + layered compositor); missing media reports once per
 * run and the transport continues. With @force the same-frame dedup is
 * bypassed — the paused repaint path needs a fresh render even though
 * the frame index did not move. */
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
          if (self->frame_func != NULL)
            self->frame_func (self, NULL, self->frame_data);
        }
      return;
    }

  const gint64 frame_index = oe_time_us_to_frame (position_us, self->sequence.frame_rate);

  if (!force && self->video_active && frame_index == self->video_frame_index)
    return; /* same frame — stills decode once per source frame */

  OeRenderSession *render = ensure_render_session (self);

  if (render == NULL)
    {
      if (!self->video_missing_reported)
        {
          self->video_missing_reported = TRUE;
          fire_event (self, OE_PLAYBACK_EVENT_MISSING_MEDIA_SKIPPED,
                      "cannot open any media for the sequence");
        }
      return;
    }

  GError *error = NULL;
  guint8 *canvas = oe_render_session_frame_at (render, position_us, VIDEO_DECODE_BOX_W,
                                               VIDEO_DECODE_BOX_H, &error);

  if (canvas == NULL)
    {
      if (!self->video_missing_reported)
        {
          self->video_missing_reported = TRUE;
          fire_event (self, OE_PLAYBACK_EVENT_MISSING_MEDIA_SKIPPED, error->message);
        }
      g_error_free (error);
      return;
    }

  OePlaybackVideoFrame *frame = g_new0 (OePlaybackVideoFrame, 1);

  frame->source_us = map.source_us;
  frame->width = VIDEO_DECODE_BOX_W;
  frame->height = VIDEO_DECODE_BOX_H;
  frame->rgba = canvas;

  self->video_active = TRUE;
  self->video_frame_index = frame_index;

  if (self->frame_func != NULL)
    self->frame_func (self, frame, self->frame_data); /* ownership transfers */
  else
    oe_playback_video_frame_free (frame);
}

static void
update_video (OePlaybackSession *self, gint64 position_us)
{
  update_video_full (self, position_us, FALSE);
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

  update_video_full (session, session->last_position_us, TRUE);
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
  self->anchor_time_us = session_now (self);

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
  close_render_session (session);

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

  fire_notify (session, clamped);
}

gint64
oe_playback_session_tick (OePlaybackSession *session)
{
  g_return_val_if_fail (session != NULL, g_get_monotonic_time ());

  const gint64 now = session_now (session);

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

  return compute_position ((OePlaybackSession *) session, session_now (session));
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
