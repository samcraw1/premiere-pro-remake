/* oe_project.c — the project & timeline data model (Phase 3).
 *
 * Single-threaded by contract, like the media library: constructed,
 * mutated, and freed on the main thread, so the observer needs no
 * locking and fires exactly once per mutation, inline on the caller.
 *
 * Destruction order (dispose): the sequence's tracks — each owning its
 * clips — go first, then the media references with their paths, then
 * the name. The observer is a plain pointer pair and is never invoked
 * during teardown: only mutations fire it, so an owner can never see
 * its project change from inside dispose.
 */

#include "oe_project.h"

#include <string.h>

typedef struct
{
  guint ref;
  gchar *path;
  /* Session-state trim bound (oe_project_set_media_source_duration):
   * probed AV duration, 0 for a still or not-yet-probed media. Never
   * serialized — probe metadata is regenerable (project-format.md). */
  gint64 source_duration_us;
} MediaRef;

struct _OeProject
{
  GObject parent_instance;

  gchar *name;
  OeSequence sequence; /* owns the tracks (OeTrack*), which own the clips (OeClip*) */
  GPtrArray *media;    /* MediaRef*, in insertion order */
  guint next_media_ref;
  OeProjectChangedFunc observer;
  gpointer observer_data;
};

G_DEFINE_TYPE (OeProject, oe_project, G_TYPE_OBJECT)

/* Forward decls: the value-type trio below allocates the same heap
 * tracks the live sequence stores, and needs their free func first. */
static OeTrack *track_new (OeTrackKind kind);
static void track_free (gpointer data);
static OeTrack *track_at (OeProject *self, guint track_index);

G_DEFINE_QUARK (oe - project - error, oe_project_error)

/* ------------------------------------------------------------------ */
/* Model value types: init / clear / copy (deep).                      */
/* ------------------------------------------------------------------ */

static OeClip *
clip_new (gint64 position_us, gint64 source_in_us, gint64 source_out_us, guint media_ref)
{
  OeClip *clip = g_new0 (OeClip, 1);

  clip->position_us = position_us;
  clip->source_in_us = source_in_us;
  clip->source_out_us = source_out_us;
  clip->media_ref = media_ref;
  return clip;
}

static void
clip_free (gpointer data)
{
  g_free (data);
}

static gint
clip_compare (gconstpointer a, gconstpointer b)
{
  const OeClip *ca = *(const OeClip *const *) a;
  const OeClip *cb = *(const OeClip *const *) b;

  return (ca->position_us > cb->position_us) - (ca->position_us < cb->position_us);
}

void
oe_track_init (OeTrack *track)
{
  g_return_if_fail (track != NULL);

  memset (track, 0, sizeof (*track));
  track->clips = g_ptr_array_new_with_free_func (clip_free);
}

void
oe_track_clear (OeTrack *track)
{
  g_return_if_fail (track != NULL);

  g_clear_pointer (&track->clips, g_ptr_array_unref);
  memset (track, 0, sizeof (*track));
}

void
oe_track_copy (OeTrack *dst, const OeTrack *src)
{
  g_return_if_fail (dst != NULL);
  g_return_if_fail (src != NULL);

  oe_track_clear (dst);
  dst->kind = src->kind;
  dst->clips = g_ptr_array_new_full (src->clips->len, clip_free);

  for (guint i = 0; i < src->clips->len; i++)
    {
      const OeClip *clip = g_ptr_array_index (src->clips, i);

      g_ptr_array_add (dst->clips, clip_new (clip->position_us, clip->source_in_us,
                                             clip->source_out_us, clip->media_ref));
    }
}

void
oe_sequence_init (OeSequence *sequence)
{
  g_return_if_fail (sequence != NULL);

  memset (sequence, 0, sizeof (*sequence));
  sequence->frame_rate = (OeRational) { 0, 0 };
  sequence->width = OE_SEQUENCE_DEFAULT_WIDTH;
  sequence->height = OE_SEQUENCE_DEFAULT_HEIGHT;
  sequence->tracks = g_ptr_array_new_with_free_func ((GDestroyNotify) track_free);
}

void
oe_sequence_clear (OeSequence *sequence)
{
  g_return_if_fail (sequence != NULL);

  g_clear_pointer (&sequence->tracks, g_ptr_array_unref);
  memset (sequence, 0, sizeof (*sequence));
}

void
oe_sequence_copy (OeSequence *dst, const OeSequence *src)
{
  g_return_if_fail (dst != NULL);
  g_return_if_fail (src != NULL);

  oe_sequence_clear (dst);
  dst->frame_rate = src->frame_rate;
  dst->width = src->width;
  dst->height = src->height;
  dst->tracks = g_ptr_array_new_full (src->tracks->len, (GDestroyNotify) track_free);

  for (guint i = 0; i < src->tracks->len; i++)
    {
      OeTrack *track = g_new0 (OeTrack, 1);

      oe_track_copy (track, g_ptr_array_index (src->tracks, i));
      g_ptr_array_add (dst->tracks, track);
    }
}

/* Heap track used both inside the live sequence and inside deep
 * copies; the owning GPtrArray's free func releases it. */
static OeTrack *
track_new (OeTrackKind kind)
{
  OeTrack *track = g_new0 (OeTrack, 1);

  oe_track_init (track);
  track->kind = kind;
  return track;
}

static void
track_free (gpointer data)
{
  OeTrack *track = data;

  oe_track_clear (track);
  g_free (track);
}

const gchar *
oe_track_kind_get_name (OeTrackKind kind)
{
  switch (kind)
    {
    case OE_TRACK_VIDEO:
      return "video";
    case OE_TRACK_AUDIO:
      return "audio";
    default:
      return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Media references.                                                   */
/* ------------------------------------------------------------------ */

static MediaRef *
media_ref_new (guint ref, const gchar *path)
{
  MediaRef *media = g_new0 (MediaRef, 1);

  media->ref = ref;
  media->path = g_strdup (path);
  return media;
}

static void
media_ref_free (gpointer data)
{
  MediaRef *media = data;

  g_clear_pointer (&media->path, g_free);
  g_free (media);
}

static MediaRef *
find_media_by_ref (OeProject *self, guint ref)
{
  for (guint i = 0; i < self->media->len; i++)
    {
      MediaRef *media = g_ptr_array_index (self->media, i);

      if (media->ref == ref)
        return media;
    }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Observer + lifecycle.                                               */
/* ------------------------------------------------------------------ */

static void
notify (OeProject *self)
{
  if (self->observer != NULL)
    self->observer (self->observer_data);
}

static void
oe_project_init (OeProject *self)
{
  self->name = g_strdup ("Untitled");
  self->sequence.frame_rate = (OeRational) { 0, 0 };
  self->sequence.width = OE_SEQUENCE_DEFAULT_WIDTH;
  self->sequence.height = OE_SEQUENCE_DEFAULT_HEIGHT;
  self->sequence.tracks = g_ptr_array_new_with_free_func ((GDestroyNotify) track_free);
  self->media = g_ptr_array_new_with_free_func (media_ref_free);
  self->next_media_ref = 1;
}

static void
oe_project_dispose (GObject *object)
{
  OeProject *self = OE_PROJECT (object);

  /* Destruction order: tracks (owning clips) first, then the media
   * references (owning paths), then the name. The observer is a plain
   * pointer pair — dispose never invokes it, so an owner cannot
   * observe its own project's teardown. */
  g_clear_pointer (&self->sequence.tracks, g_ptr_array_unref);
  g_clear_pointer (&self->media, g_ptr_array_unref);
  g_clear_pointer (&self->name, g_free);

  G_OBJECT_CLASS (oe_project_parent_class)->dispose (object);
}

static void
oe_project_class_init (OeProjectClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = oe_project_dispose;
}

OeProject *
oe_project_new (OeRational frame_rate)
{
  g_return_val_if_fail (frame_rate.num > 0 && frame_rate.den > 0, NULL);

  OeProject *self = g_object_new (OE_TYPE_PROJECT, NULL);

  self->sequence.frame_rate = frame_rate;
  return self;
}

OeProject *
oe_project_new_default (void)
{
  return oe_project_new ((OeRational) { OE_PROJECT_DEFAULT_RATE_NUM, OE_PROJECT_DEFAULT_RATE_DEN });
}

void
oe_project_set_observer (OeProject *self, OeProjectChangedFunc observer, gpointer user_data)
{
  g_return_if_fail (OE_IS_PROJECT (self));

  self->observer = observer;
  self->observer_data = user_data;
}

/* ------------------------------------------------------------------ */
/* Name.                                                               */
/* ------------------------------------------------------------------ */

const gchar *
oe_project_get_name (OeProject *self)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), NULL);

  return self->name;
}

void
oe_project_set_name (OeProject *self, const gchar *name)
{
  g_return_if_fail (OE_IS_PROJECT (self));
  g_return_if_fail (name != NULL);

  if (g_strcmp0 (self->name, name) == 0)
    return;

  g_free (self->name);
  self->name = g_strdup (name);
  notify (self);
}

/* ------------------------------------------------------------------ */
/* Sequence reads (deep copies).                                       */
/* ------------------------------------------------------------------ */

void
oe_project_get_sequence (OeProject *self, OeSequence *out)
{
  g_return_if_fail (OE_IS_PROJECT (self));
  g_return_if_fail (out != NULL);

  /* Build a fresh local and hand it over whole: like the media
   * library's get(), @out is never cleared here — it must be
   * uninitialized or zeroed caller storage. */
  OeSequence copy;

  oe_sequence_init (&copy);
  copy.frame_rate = self->sequence.frame_rate;
  copy.width = self->sequence.width;
  copy.height = self->sequence.height;

  for (guint i = 0; i < self->sequence.tracks->len; i++)
    {
      OeTrack *track = g_new0 (OeTrack, 1);

      oe_track_copy (track, g_ptr_array_index (self->sequence.tracks, i));
      g_ptr_array_add (copy.tracks, track);
    }

  *out = copy;
}

gboolean
oe_project_set_sequence_size (OeProject *self, gint width, gint height, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  /* Even dimensions are a model-level rule: the export path emits
   * yuv420p, whose chroma planes need them. */
  if (width <= 0 || height <= 0 || (width % 2) != 0 || (height % 2) != 0)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_SIZE,
                   "sequence size must be positive and even (got %dx%d)", width, height);
      return FALSE;
    }

  if (self->sequence.width == width && self->sequence.height == height)
    return TRUE;

  self->sequence.width = width;
  self->sequence.height = height;
  notify (self);
  return TRUE;
}

guint
oe_project_get_track_count (OeProject *self)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), 0);

  return self->sequence.tracks->len;
}

guint
oe_project_get_clip_count (OeProject *self, guint track_index)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), 0);

  const OeTrack *track = track_at (self, track_index);

  return track != NULL ? track->clips->len : 0;
}

gboolean
oe_project_get_clip (OeProject *self, guint track_index, guint clip_index, OeClip *out)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);
  g_return_val_if_fail (out != NULL, FALSE);

  const OeTrack *track = track_at (self, track_index);

  if (track == NULL || clip_index >= track->clips->len)
    return FALSE;

  *out = *(const OeClip *) g_ptr_array_index (track->clips, clip_index);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Mutations: tracks.                                                  */
/* ------------------------------------------------------------------ */

guint
oe_project_add_track (OeProject *self, OeTrackKind kind)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), 0);

  OeTrack *track = track_new (kind);

  g_ptr_array_add (self->sequence.tracks, track);
  notify (self);
  return self->sequence.tracks->len - 1;
}

/* ------------------------------------------------------------------ */
/* Mutations: clips.                                                   */
/* ------------------------------------------------------------------ */

/* A placement [position, position + duration) must not intersect any
 * existing clip on the track. Positions and durations are
 * non-negative, so the intersection test stays in integer arithmetic:
 * a.start < b.end && b.start < a.end. */
static gboolean
placement_overlaps (const OeTrack *track, gint64 position, gint64 duration, const OeClip *exclude)
{
  for (guint i = 0; i < track->clips->len; i++)
    {
      const OeClip *clip = g_ptr_array_index (track->clips, i);

      if (clip == exclude)
        continue;

      gint64 clip_end = clip->position_us + (clip->source_out_us - clip->source_in_us);

      if (clip->position_us < position + duration && position < clip_end)
        return TRUE;
    }
  return FALSE;
}

static OeTrack *
track_at (OeProject *self, guint track_index)
{
  if (track_index >= self->sequence.tracks->len)
    return NULL;

  return g_ptr_array_index (self->sequence.tracks, track_index);
}

static gboolean
validate_placement (OeProject *self, guint track_index, guint media_ref, gint64 position_us,
                    gint64 source_in_us, gint64 source_out_us, OeTrack **track_out, GError **error)
{
  OeTrack *track = track_at (self, track_index);

  if (track == NULL)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK,
                   "track %u does not exist (the project has %u)", track_index,
                   self->sequence.tracks->len);
      return FALSE;
    }

  if (find_media_by_ref (self, media_ref) == NULL)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_UNKNOWN_MEDIA,
                   "media reference %u is not in the project", media_ref);
      return FALSE;
    }

  if (position_us < 0 || source_in_us < 0 || source_out_us <= source_in_us)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE,
                   "invalid placement: position %lld, source range [%lld, %lld) — "
                   "position and source-in must be >= 0 and source-out must exceed source-in",
                   (long long) position_us, (long long) source_in_us, (long long) source_out_us);
      return FALSE;
    }

  gint64 duration = source_out_us - source_in_us;

  if (position_us > G_MAXINT64 - duration)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE,
                   "placement at %lld with duration %lld overflows the timeline",
                   (long long) position_us, (long long) duration);
      return FALSE;
    }

  *track_out = track;
  return TRUE;
}

gboolean
oe_project_insert_clip (OeProject *self, guint track_index, guint media_ref, gint64 position_us,
                        gint64 source_in_us, gint64 source_out_us, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  OeTrack *track = NULL;

  if (!validate_placement (self, track_index, media_ref, position_us, source_in_us, source_out_us,
                           &track, error))
    return FALSE;

  gint64 duration = source_out_us - source_in_us;

  if (placement_overlaps (track, position_us, duration, NULL))
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP,
                   "clip [%lld, %lld) overlaps an existing clip on %s track %u",
                   (long long) position_us, (long long) (position_us + duration),
                   oe_track_kind_get_name (track->kind), track_index);
      return FALSE;
    }

  OeClip *clip = clip_new (position_us, source_in_us, source_out_us, media_ref);

  g_ptr_array_add (track->clips, clip);
  g_ptr_array_sort (track->clips, clip_compare);
  notify (self);

  return TRUE;
}

gboolean
oe_project_move_clip (OeProject *self, guint track_index, guint clip_index, gint64 position_us,
                      GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  OeTrack *track = track_at (self, track_index);

  if (track == NULL)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK,
                   "track %u does not exist (the project has %u)", track_index,
                   self->sequence.tracks->len);
      return FALSE;
    }

  if (clip_index >= track->clips->len)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP,
                   "clip %u does not exist on track %u (it has %u)", clip_index, track_index,
                   track->clips->len);
      return FALSE;
    }

  OeClip *clip = g_ptr_array_index (track->clips, clip_index);

  if (position_us < 0)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE,
                   "clip position must be >= 0 (got %lld)", (long long) position_us);
      return FALSE;
    }

  gint64 duration = clip->source_out_us - clip->source_in_us;

  if (position_us > G_MAXINT64 - duration)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE,
                   "placement at %lld with duration %lld overflows the timeline",
                   (long long) position_us, (long long) duration);
      return FALSE;
    }

  /* The moved clip's own footprint is not an overlap target. */
  if (placement_overlaps (track, position_us, duration, clip))
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP,
                   "move to [%lld, %lld) overlaps an existing clip on %s track %u",
                   (long long) position_us, (long long) (position_us + duration),
                   oe_track_kind_get_name (track->kind), track_index);
      return FALSE;
    }

  clip->position_us = position_us;
  g_ptr_array_sort (track->clips, clip_compare);
  notify (self);

  return TRUE;
}

gboolean
oe_project_remove_clip (OeProject *self, guint track_index, guint clip_index, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  OeTrack *track = track_at (self, track_index);

  if (track == NULL)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK,
                   "track %u does not exist (the project has %u)", track_index,
                   self->sequence.tracks->len);
      return FALSE;
    }

  if (clip_index >= track->clips->len)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP,
                   "clip %u does not exist on track %u (it has %u)", clip_index, track_index,
                   track->clips->len);
      return FALSE;
    }

  g_ptr_array_remove_index (track->clips, clip_index);
  notify (self);

  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Mutations: clips (cont.) — trims.                                   */
/* ------------------------------------------------------------------ */

gboolean
oe_project_trim_clip (OeProject *self, guint track_index, guint clip_index, gint64 new_source_in,
                      gint64 new_source_out, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  OeTrack *track = track_at (self, track_index);

  if (track == NULL)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK,
                   "track %u does not exist (the project has %u)", track_index,
                   self->sequence.tracks->len);
      return FALSE;
    }

  if (clip_index >= track->clips->len)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP,
                   "clip %u does not exist on track %u (it has %u)", clip_index, track_index,
                   track->clips->len);
      return FALSE;
    }

  OeClip *clip = g_ptr_array_index (track->clips, clip_index);

  if (new_source_in < 0 || new_source_out <= new_source_in)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE,
                   "invalid trim: source range [%lld, %lld) — source-in must be >= 0 and "
                   "source-out must exceed source-in",
                   (long long) new_source_in, (long long) new_source_out);
      return FALSE;
    }

  MediaRef *media = find_media_by_ref (self, clip->media_ref);

  if (media == NULL)
    {
      /* Unreachable through the public API (insert validates the ref),
       * but a clip naming unknown media must not trim as if unbounded. */
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_UNKNOWN_MEDIA,
                   "media reference %u is not in the project", clip->media_ref);
      return FALSE;
    }

  /* AV media is bounded by its probed source duration; stills (duration
   * 0 / never annotated) extend freely — their source range encodes
   * screen duration, not a probe result (uniform-duration rule). */
  if (media->source_duration_us > 0 && new_source_out > media->source_duration_us)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE,
                   "trim to source range [%lld, %lld) exceeds the probed media duration %lld",
                   (long long) new_source_in, (long long) new_source_out,
                   (long long) media->source_duration_us);
      return FALSE;
    }

  gint64 duration = new_source_out - new_source_in;

  if (clip->position_us > G_MAXINT64 - duration)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE,
                   "trim to duration %lld at position %lld overflows the timeline",
                   (long long) duration, (long long) clip->position_us);
      return FALSE;
    }

  /* Position is untouched, but the duration change can grow the
   * footprint across a neighbour: the trimmed clip's own footprint is
   * not an overlap target, other clips are (same rule as a move).
   * Positions stay distinct and ordered — no re-sort needed. */
  if (placement_overlaps (track, clip->position_us, duration, clip))
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_OVERLAP,
                   "trim to [%lld, %lld) overlaps an existing clip on %s track %u",
                   (long long) clip->position_us, (long long) (clip->position_us + duration),
                   oe_track_kind_get_name (track->kind), track_index);
      return FALSE;
    }

  clip->source_in_us = new_source_in;
  clip->source_out_us = new_source_out;
  notify (self);

  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Mutations: media references.                                        */
/* ------------------------------------------------------------------ */

static void
insert_media (OeProject *self, guint ref, const gchar *path)
{
  g_ptr_array_add (self->media, media_ref_new (ref, path));

  /* Keep the automatic counter past every file-stable number the
   * document brought in, so later add_media() calls never collide. */
  if (ref >= self->next_media_ref)
    self->next_media_ref = (ref == G_MAXUINT32) ? G_MAXUINT32 : ref + 1;

  notify (self);
}

guint
oe_project_add_media (OeProject *self, const gchar *path)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), 0);
  g_return_val_if_fail (path != NULL && path[0] != '\0', 0);
  g_return_val_if_fail (self->next_media_ref > 0, 0); /* counter exhausted */

  guint ref = self->next_media_ref;

  insert_media (self, ref, path);
  return ref;
}

gboolean
oe_project_add_media_ref (OeProject *self, guint ref, const gchar *path, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);
  g_return_val_if_fail (path != NULL && path[0] != '\0', FALSE);
  g_return_val_if_fail (ref > 0, FALSE);

  if (find_media_by_ref (self, ref) != NULL)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_DUPLICATE_REF,
                   "media reference %u is already in the project", ref);
      return FALSE;
    }

  insert_media (self, ref, path);
  return TRUE;
}

guint
oe_project_get_media_count (OeProject *self)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), 0);

  return self->media->len;
}

gboolean
oe_project_get_media (OeProject *self, guint index, guint *ref, gchar **path)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  if (index >= self->media->len)
    return FALSE;

  MediaRef *media = g_ptr_array_index (self->media, index);

  if (ref != NULL)
    *ref = media->ref;
  if (path != NULL)
    *path = g_strdup (media->path);
  return TRUE;
}

gchar *
oe_project_dup_media_path (OeProject *self, guint ref)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), NULL);

  MediaRef *media = find_media_by_ref (self, ref);

  return media != NULL ? g_strdup (media->path) : NULL;
}

void
oe_project_set_media_source_duration (OeProject *self, guint ref, gint64 source_duration_us)
{
  g_return_if_fail (OE_IS_PROJECT (self));

  /* Session-state annotation, not a document mutation: it never fires
   * the observer (unknown refs are silently ignored, like the media
   * library's set_thumbnail). */
  MediaRef *media = find_media_by_ref (self, ref);

  if (media == NULL)
    return;

  media->source_duration_us = source_duration_us;
}

gboolean
oe_project_get_media_source_duration (OeProject *self, guint ref, gint64 *source_duration_us)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  MediaRef *media = find_media_by_ref (self, ref);

  if (media == NULL)
    return FALSE;

  if (source_duration_us != NULL)
    *source_duration_us = media->source_duration_us;
  return TRUE;
}
