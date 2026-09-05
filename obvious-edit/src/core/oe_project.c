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

OeClipVisual
oe_clip_visual_identity (void)
{
  OeClipVisual visual;

  memset (&visual, 0, sizeof (visual));
  visual.scale_permille = 1000;
  visual.opacity = 255;
  return visual;
}

void
oe_clip_visual_clear (OeClipVisual *visual)
{
  g_return_if_fail (visual != NULL);

  g_clear_pointer (&visual->keyframes, g_array_unref);
  memset (visual, 0, sizeof (*visual));
}

void
oe_clip_visual_copy (OeClipVisual *dst, const OeClipVisual *src)
{
  g_return_if_fail (dst != NULL);
  g_return_if_fail (src != NULL);

  /* Release the destination's owned store first, then copy fields and
   * clone the source's store — the two visuals never share memory. */
  g_clear_pointer (&dst->keyframes, g_array_unref);
  *dst = *src;
  dst->keyframes = oe_keyframes_copy_array (src->keyframes);
}

gboolean
oe_clip_visual_equal (const OeClipVisual *a, const OeClipVisual *b)
{
  g_return_val_if_fail (a != NULL, FALSE);
  g_return_val_if_fail (b != NULL, FALSE);

  if (!oe_keyframes_equal (a->keyframes, b->keyframes))
    return FALSE;

  return a->pos_x == b->pos_x && a->pos_y == b->pos_y && a->scale_permille == b->scale_permille
         && a->rotation_cdeg == b->rotation_cdeg && a->opacity == b->opacity
         && a->crop_l == b->crop_l && a->crop_t == b->crop_t && a->crop_r == b->crop_r
         && a->crop_b == b->crop_b && a->fade_in_us == b->fade_in_us
         && a->fade_out_us == b->fade_out_us;
}

gboolean
oe_clip_visual_is_default (const OeClipVisual *visual)
{
  OeClipVisual identity;

  g_return_val_if_fail (visual != NULL, FALSE);

  identity = oe_clip_visual_identity ();
  return oe_clip_visual_equal (visual, &identity);
}

void
oe_clip_visual_resolve (const OeClipVisual *visual, gint64 clip_time_us, OeClipVisual *out)
{
  g_return_if_fail (visual != NULL);
  g_return_if_fail (out != NULL);

  *out = *visual;
  out->keyframes = NULL; /* the resolved visual is transient, owns nothing */

  out->opacity
      = oe_keyframes_sample (visual->keyframes, OE_KEYFRAME_OPACITY, clip_time_us, visual->opacity);
  out->pos_x
      = oe_keyframes_sample (visual->keyframes, OE_KEYFRAME_POS_X, clip_time_us, visual->pos_x);
  out->pos_y
      = oe_keyframes_sample (visual->keyframes, OE_KEYFRAME_POS_Y, clip_time_us, visual->pos_y);
  out->scale_permille = oe_keyframes_sample (visual->keyframes, OE_KEYFRAME_SCALE_PERMILLE,
                                             clip_time_us, visual->scale_permille);
  out->rotation_cdeg = oe_keyframes_sample (visual->keyframes, OE_KEYFRAME_ROTATION_CDEG,
                                            clip_time_us, visual->rotation_cdeg);
}

gboolean
oe_clip_visual_is_valid (const OeClipVisual *visual)
{
  g_return_val_if_fail (visual != NULL, FALSE);

  if (!oe_keyframes_valid (visual->keyframes))
    return FALSE;
  if (visual->scale_permille < 1 || visual->scale_permille > 32000)
    return FALSE;
  if (visual->rotation_cdeg < -36000 || visual->rotation_cdeg > 36000)
    return FALSE;
  if (visual->crop_l > G_MAXINT || visual->crop_t > G_MAXINT || visual->crop_r > G_MAXINT
      || visual->crop_b > G_MAXINT)
    return FALSE;
  if (visual->fade_in_us > (guint64) G_MAXINT64 || visual->fade_out_us > (guint64) G_MAXINT64)
    return FALSE;
  return TRUE; /* opacity: guint8 covers the full 0-255 domain */
}

static OeClip *
clip_new (gint64 position_us, gint64 source_in_us, gint64 source_out_us, guint media_ref)
{
  OeClip *clip = g_new0 (OeClip, 1);

  clip->position_us = position_us;
  clip->source_in_us = source_in_us;
  clip->source_out_us = source_out_us;
  clip->media_ref = media_ref;
  clip->visual = oe_clip_visual_identity ();
  return clip;
}

static void
clip_free (gpointer data)
{
  OeClip *clip = data;

  oe_clip_visual_clear (&clip->visual);
  g_free (clip);
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
      OeClip *copy
          = clip_new (clip->position_us, clip->source_in_us, clip->source_out_us, clip->media_ref);

      /* Deep-copy the owned visual too: track copies must never alias
       * clip state (sequence snapshots hand these to the renderer). */
      oe_clip_visual_copy (&copy->visual, &clip->visual);
      g_ptr_array_add (dst->clips, copy);
    }
}

static void
transition_free (gpointer data)
{
  g_free (data);
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
  sequence->transitions = g_ptr_array_new_with_free_func ((GDestroyNotify) transition_free);
}

void
oe_sequence_clear (OeSequence *sequence)
{
  g_return_if_fail (sequence != NULL);

  g_clear_pointer (&sequence->tracks, g_ptr_array_unref);
  g_clear_pointer (&sequence->transitions, g_ptr_array_unref);
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

  dst->transitions = g_ptr_array_new_full (src->transitions->len, (GDestroyNotify) transition_free);

  for (guint i = 0; i < src->transitions->len; i++)
    {
      OeTransition *copy = g_new0 (OeTransition, 1);

      *copy = *(OeTransition *) g_ptr_array_index (src->transitions, i);
      g_ptr_array_add (dst->transitions, copy);
    }
}

const gchar *
oe_transition_kind_get_name (OeTransitionKind kind)
{
  switch (kind)
    {
    case OE_TRANSITION_CROSS_DISSOLVE:
      return "cross-dissolve";
    case OE_TRANSITION_DIP_TO_BLACK:
      return "dip-to-black";
    default:
      return "unknown";
    }
}

static gint64
clip_length (const OeClip *clip)
{
  return clip->source_out_us - clip->source_in_us;
}

OeTransitionWindow
oe_transition_window (const OeSequence *sequence, const OeTransition *t)
{
  OeTransitionWindow w = { 0 };

  g_return_val_if_fail (sequence != NULL, w);
  g_return_val_if_fail (t != NULL, w);

  if (t->track_index >= sequence->tracks->len || t->duration_us <= 0 || t->at_us <= 0)
    return w;

  const OeTrack *track = g_ptr_array_index (sequence->tracks, t->track_index);

  if (track->kind != OE_TRACK_VIDEO)
    return w;

  /* The boundary contract: one clip ends exactly at @at_us, the next
   * starts exactly there. Linear scan is fine — track clip counts are
   * small and the compositor calls this per rendered frame only while
   * a window is plausibly active. */
  const OeClip *out = NULL;
  const OeClip *in = NULL;

  for (guint i = 0; i < track->clips->len; i++)
    {
      const OeClip *clip = g_ptr_array_index (track->clips, i);

      if (clip->position_us + clip_length (clip) == t->at_us)
        out = clip;
      if (clip->position_us == t->at_us)
        in = clip;
    }

  if (out == NULL || in == NULL)
    return w;

  const gint64 start = t->at_us - t->duration_us / 2;
  const gint64 end = start + t->duration_us;

  /* Both neighbors must still cover the stored window; a moved or
   * trimmed clip degrades the transition to the straight cut (the
   * caller renders no blend, no fixup anywhere). */
  if (start < out->position_us || end - t->at_us > clip_length (in))
    return w;

  w.active = TRUE;
  w.start_us = start;
  w.end_us = end;
  w.out_clip = out;
  w.in_clip = in;
  return w;
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
  self->sequence.transitions = g_ptr_array_new_with_free_func ((GDestroyNotify) transition_free);
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
  g_clear_pointer (&self->sequence.transitions, g_ptr_array_unref);
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
  oe_sequence_copy (&copy, &self->sequence);
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

gboolean
oe_project_set_clip_visual (OeProject *self, guint track_index, guint clip_index,
                            const OeClipVisual *visual, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);
  g_return_val_if_fail (visual != NULL, FALSE);

  OeTrack *track = track_at (self, track_index);

  if (track == NULL)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK,
                   "track index %u out of range", track_index);
      return FALSE;
    }

  if (clip_index >= track->clips->len)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP,
                   "clip index %u out of range on track %u", clip_index, track_index);
      return FALSE;
    }

  if (!oe_clip_visual_is_valid (visual))
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_VISUAL,
                   "clip visual out of domain (scale %u permille, rotation %d cdeg, "
                   "opacity %u)",
                   visual->scale_permille, visual->rotation_cdeg, visual->opacity);
      return FALSE;
    }

  /* Validate first, deep-copy second, swap last: a rejected call never
   * mutates the model. The staged visual owns its own keyframe store,
   * so the swapped-out one is released exactly once. */
  OeClip *clip = g_ptr_array_index (track->clips, clip_index);
  OeClipVisual copy = oe_clip_visual_identity ();

  oe_clip_visual_copy (&copy, visual);
  oe_clip_visual_clear (&clip->visual);
  clip->visual = copy;

  notify (self);
  return TRUE;
}

gboolean
oe_project_set_clip_keyframe (OeProject *self, guint track_index, guint clip_index,
                              OeKeyframeProperty property, gint64 time_us, gint32 value,
                              GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  OeTrack *track = track_at (self, track_index);

  if (track == NULL)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK,
                   "track index %u out of range", track_index);
      return FALSE;
    }

  if (clip_index >= track->clips->len)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP,
                   "clip index %u out of range on track %u", clip_index, track_index);
      return FALSE;
    }

  if (!oe_keyframe_value_in_domain (property, value))
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_KEYFRAME,
                   "keyframe value %d out of domain for property %s", value,
                   oe_keyframe_property_get_name (property));
      return FALSE;
    }

  OeClip *clip = g_ptr_array_index (track->clips, clip_index);
  const gint64 length_us = clip->source_out_us - clip->source_in_us;

  if (time_us < 0 || time_us > length_us)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_KEYFRAME,
                   "keyframe time %" G_GINT64_FORMAT " out of the clip's [0, %" G_GINT64_FORMAT
                   "] range",
                   time_us, length_us);
      return FALSE;
    }

  /* Stage the new store fully before touching the model: a fresh array
   * (the clip may not have one yet) receives the sorted insertion, and
   * the staged store swaps in only after every check passed. */
  GArray *staged = oe_keyframes_copy_array (clip->visual.keyframes);

  if (staged == NULL)
    staged = g_array_new (FALSE, FALSE, sizeof (OeKeyframe));

  oe_keyframes_insert (staged, (OeKeyframe) { property, time_us, value });
  g_clear_pointer (&clip->visual.keyframes, g_array_unref);
  clip->visual.keyframes = staged;

  notify (self);
  return TRUE;
}

gboolean
oe_project_remove_clip_keyframe (OeProject *self, guint track_index, guint clip_index,
                                 OeKeyframeProperty property, gint64 time_us, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  OeTrack *track = track_at (self, track_index);

  if (track == NULL)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK,
                   "track index %u out of range", track_index);
      return FALSE;
    }

  if (clip_index >= track->clips->len)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_CLIP,
                   "clip index %u out of range on track %u", clip_index, track_index);
      return FALSE;
    }

  OeClip *clip = g_ptr_array_index (track->clips, clip_index);

  if (clip->visual.keyframes == NULL
      || !oe_keyframes_remove (clip->visual.keyframes, property, time_us))
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_KEYFRAME,
                   "no keyframe for property %s at time %" G_GINT64_FORMAT,
                   oe_keyframe_property_get_name (property), time_us);
      return FALSE;
    }

  /* An emptied store is equivalent to no store — release it so the
   * clip settles back to the plain static representation. */
  if (clip->visual.keyframes->len == 0)
    g_clear_pointer (&clip->visual.keyframes, g_array_unref);

  notify (self);
  return TRUE;
}

/* The boundary contract shared by add and move (D5): the outgoing clip
 * ends exactly at @at_us, the incoming clip starts exactly there, and
 * the stored duration is clamped to what both neighbors can cover —
 * the centered window must fit inside the outgoing clip's span and the
 * incoming clip's source range. Returns TRUE with @duration_us
 * clamped; FALSE with @error set when no window fits at all. */
static gboolean
transition_boundary_validate (const OeSequence *sequence, guint track_index, gint64 at_us,
                              gint64 *duration_us, GError **error)
{
  if (track_index >= sequence->tracks->len)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRACK,
                   "transition track index %u out of range", track_index);
      return FALSE;
    }

  const OeTrack *track = g_ptr_array_index (sequence->tracks, track_index);

  if (track->kind != OE_TRACK_VIDEO)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRANSITION,
                   "transitions are video-track boundaries (v1); track %u is %s", track_index,
                   oe_track_kind_get_name (track->kind));
      return FALSE;
    }

  if (at_us <= 0)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE,
                   "transition boundary %lld must be interior (sequence time > 0)",
                   (long long) at_us);
      return FALSE;
    }

  const OeClip *out = NULL;
  const OeClip *in = NULL;

  for (guint i = 0; i < track->clips->len; i++)
    {
      const OeClip *clip = g_ptr_array_index (track->clips, i);

      if (clip->position_us + clip_length (clip) == at_us)
        out = clip;
      if (clip->position_us == at_us)
        in = clip;
    }

  if (out == NULL || in == NULL)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRANSITION,
                   "no adjacent clips meet at boundary %lld on track %u", (long long) at_us,
                   track_index);
      return FALSE;
    }

  /* Window [at - dur/2, at + dur - dur/2) fits: start >= out->position
   * caps dur at 2*(at - out->position); the incoming clip's source must
   * reach (end - at) = dur - dur/2, capping dur at 2*in_len. */
  const gint64 max_dur
      = MIN (2 * (at_us - out->position_us), 2 * (in->source_out_us - in->source_in_us));

  if (max_dur <= 0)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRANSITION,
                   "boundary %lld supports no transition window", (long long) at_us);
      return FALSE;
    }

  if (*duration_us <= 0)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_RANGE,
                   "transition duration %lld must be positive", (long long) *duration_us);
      return FALSE;
    }

  /* Clamped to both clips: the stored value is what the timeline can
   * actually cover, never a pending failure. */
  *duration_us = MIN (*duration_us, max_dur);
  return TRUE;
}

guint
oe_project_get_transition_count (OeProject *self)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), 0);
  g_return_val_if_fail (self->sequence.transitions != NULL, 0);

  return self->sequence.transitions->len;
}

gboolean
oe_project_get_transition (OeProject *self, guint index, OeTransition *out)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);
  g_return_val_if_fail (out != NULL, FALSE);

  if (index >= self->sequence.transitions->len)
    return FALSE;

  *out = *(OeTransition *) g_ptr_array_index (self->sequence.transitions, index);
  return TRUE;
}

gboolean
oe_project_add_transition (OeProject *self, const OeTransition *t, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);
  g_return_val_if_fail (t != NULL, FALSE);

  OeTransition stored = *t;

  if (!transition_boundary_validate (&self->sequence, stored.track_index, stored.at_us,
                                     &stored.duration_us, error))
    return FALSE;

  OeTransition *copy = g_new0 (OeTransition, 1);

  *copy = stored;
  g_ptr_array_add (self->sequence.transitions, copy);
  notify (self);
  return TRUE;
}

gboolean
oe_project_move_transition (OeProject *self, guint index, gint64 at_us, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  if (index >= self->sequence.transitions->len)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRANSITION,
                   "transition index %u out of range", index);
      return FALSE;
    }

  OeTransition *t = g_ptr_array_index (self->sequence.transitions, index);
  gint64 duration = t->duration_us;

  if (!transition_boundary_validate (&self->sequence, t->track_index, at_us, &duration, error))
    return FALSE;

  t->at_us = at_us;
  t->duration_us = duration;
  notify (self);
  return TRUE;
}

gboolean
oe_project_remove_transition (OeProject *self, guint index, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (self), FALSE);

  if (index >= self->sequence.transitions->len)
    {
      g_set_error (error, OE_PROJECT_ERROR, OE_PROJECT_ERROR_BAD_TRANSITION,
                   "transition index %u out of range", index);
      return FALSE;
    }

  g_ptr_array_remove_index (self->sequence.transitions, index);
  notify (self);
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
