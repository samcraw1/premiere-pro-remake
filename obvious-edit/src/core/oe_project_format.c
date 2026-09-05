/* oe_project_format.c — versioned JSON v1 project files (Phase 3).
 *
 * The serializer is hand-rolled on purpose: member order (format-version
 * first), integer-only numbers, and deterministic byte output are part
 * of the contract, and a GString gives all three without depending on
 * generator-side options. Strings are JSON-escaped; UTF-8 passes
 * through raw.
 *
 * The loader is strict v1 and validates the whole document before
 * constructing any model object, so a failed load can never return a
 * half-built project. Every defect is reported with its location
 * (e.g. "tracks[0].clips[2]: ...").
 *
 * Saving goes through write_atomic(): temp file in the target
 * directory, full write, fsync, rename over the target only on
 * success. Any failure unlinks the temp and leaves a pre-existing
 * target byte-identical.
 */

#include "oe_project_format.h"

#include <errno.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "oe_time.h"

G_DEFINE_QUARK (oe - project - format - error, oe_project_format_error)

#define ROOT_KEY "obvious-edit-project"

/* ------------------------------------------------------------------ */
/* Serialization.                                                      */
/* ------------------------------------------------------------------ */

static void
append_escaped (GString *out, const gchar *s)
{
  for (const gchar *p = s; *p != '\0'; p++)
    {
      guchar c = (guchar) *p;

      switch (c)
        {
        case '"':
          g_string_append (out, "\\\"");
          break;
        case '\\':
          g_string_append (out, "\\\\");
          break;
        case '\b':
          g_string_append (out, "\\b");
          break;
        case '\f':
          g_string_append (out, "\\f");
          break;
        case '\n':
          g_string_append (out, "\\n");
          break;
        case '\r':
          g_string_append (out, "\\r");
          break;
        case '\t':
          g_string_append (out, "\\t");
          break;
        default:
          if (c < 0x20)
            g_string_append_printf (out, "\\u%04x", c);
          else
            g_string_append_c (out, (gchar) c);
        }
    }
}

static void
append_quoted (GString *out, const gchar *s)
{
  g_string_append_c (out, '"');
  append_escaped (out, s);
  g_string_append_c (out, '"');
}

static void
append_int (GString *out, gint64 v)
{
  g_string_append_printf (out, "%" G_GINT64_FORMAT, v);
}

/* The clip-level visual member (Phase 9 Wave A): written always —
 * every field, every clip — so a current writer's output is stable and
 * a save-load-save round trip is byte-identical. Readers backfill the
 * identity when the member is absent (pre-Phase-9 file). Integer
 * tokens only: the fixed-point fields never serialize as floats. */
static void
append_clip_visual (GString *out, const OeClipVisual *v)
{
  g_string_append (out, ",\n          \"visual\": {\n");
  g_string_append (out, "            \"pos-x\": ");
  append_int (out, v->pos_x);
  g_string_append (out, ",\n            \"pos-y\": ");
  append_int (out, v->pos_y);
  g_string_append (out, ",\n            \"scale-permille\": ");
  append_int (out, (gint64) v->scale_permille);
  g_string_append (out, ",\n            \"rotation-cdeg\": ");
  append_int (out, v->rotation_cdeg);
  g_string_append (out, ",\n            \"opacity\": ");
  append_int (out, v->opacity);
  g_string_append (out, ",\n            \"crop-l\": ");
  append_int (out, (gint64) v->crop_l);
  g_string_append (out, ",\n            \"crop-t\": ");
  append_int (out, (gint64) v->crop_t);
  g_string_append (out, ",\n            \"crop-r\": ");
  append_int (out, (gint64) v->crop_r);
  g_string_append (out, ",\n            \"crop-b\": ");
  append_int (out, (gint64) v->crop_b);
  g_string_append (out, ",\n            \"fade-in-us\": ");
  append_int (out, (gint64) v->fade_in_us);
  g_string_append (out, ",\n            \"fade-out-us\": ");
  append_int (out, (gint64) v->fade_out_us);
  g_string_append (out, "\n          }");
}

/* The clip-level keyframes member (Phase 9 Wave B): written always —
 * an empty object when the store is absent — so a current writer's
 * output is stable and save-load-save is byte-identical. Absent on
 * read backfills NONE (an absent member means no keyframes). The
 * store's (property, time) sort order makes the output deterministic;
 * integer tokens only. */
static void
append_clip_keyframes (GString *out, const OeClipVisual *v)
{
  g_string_append (out, ",\n          \"keyframes\": {");

  if (v->keyframes == NULL || v->keyframes->len == 0)
    {
      g_string_append (out, "}");
      return;
    }

  g_string_append (out, "\n");

  const GArray *store = v->keyframes;
  gboolean first_property = TRUE;
  guint run_start = 0;

  while (run_start < store->len)
    {
      const OeKeyframe *first = &g_array_index (store, OeKeyframe, run_start);
      guint run_end = run_start + 1;

      while (run_end < store->len
             && g_array_index (store, OeKeyframe, run_end).property == first->property)
        run_end++;

      if (!first_property)
        g_string_append (out, ",\n");
      first_property = FALSE;

      g_string_append_printf (out, "            \"%s\": [",
                              oe_keyframe_property_get_name (first->property));

      for (guint k = run_start; k < run_end; k++)
        {
          const OeKeyframe *key = &g_array_index (store, OeKeyframe, k);

          g_string_append (out, k > run_start ? ",\n" : "\n");
          g_string_append (out, "              { \"time-us\": ");
          append_int (out, key->time_us);
          g_string_append (out, ", \"value\": ");
          append_int (out, key->value);
          g_string_append (out, " }");
        }

      g_string_append (out, run_end > run_start ? "\n            ]" : "]");
      run_start = run_end;
    }

  g_string_append (out, "\n          }");
}

static gchar *
serialize_project (OeProject *project)
{
  GString *out = g_string_sized_new (4096);

  OeSequence seq;

  oe_project_get_sequence (project, &seq);
  const gchar *name = oe_project_get_name (project);
  guint media_count = oe_project_get_media_count (project);

  g_string_append (out, "{\n  \"" ROOT_KEY "\": {\n");

  /* Version first, then name, frame-rate, size, media, tracks — the
   * schema order. Integers only, rates as num/den pairs. */
  g_string_append_printf (out, "    \"format-version\": %d,\n", OE_PROJECT_FORMAT_VERSION);

  g_string_append (out, "    \"name\": ");
  append_quoted (out, name != NULL ? name : "");
  g_string_append (out, ",\n");

  g_string_append (out, "    \"frame-rate\": { \"num\": ");
  append_int (out, seq.frame_rate.num);
  g_string_append (out, ", \"den\": ");
  append_int (out, seq.frame_rate.den);
  g_string_append (out, " },\n");

  /* Sequence size (Phase 8): written always; readers backfill the
   * defaults when a pre-Phase-8 file omits them. */
  g_string_append (out, "    \"width\": ");
  append_int (out, seq.width);
  g_string_append (out, ",\n    \"height\": ");
  append_int (out, seq.height);
  g_string_append (out, ",\n");

  g_string_append (out, "    \"media\": [");
  for (guint i = 0; i < media_count; i++)
    {
      guint ref = 0;
      gchar *path = NULL;

      oe_project_get_media (project, i, &ref, &path);
      g_string_append_printf (out, "%s\n      { \"ref\": %u, \"path\": ", i > 0 ? "," : "", ref);
      append_quoted (out, path);
      g_string_append (out, " }");
      g_free (path);
    }
  if (media_count > 0)
    g_string_append (out, "\n    ");
  g_string_append (out, "],\n");

  g_string_append (out, "    \"tracks\": [");
  for (guint t = 0; t < seq.tracks->len; t++)
    {
      const OeTrack *track = g_ptr_array_index (seq.tracks, t);

      g_string_append_printf (out, "%s\n      { \"kind\": \"%s\", \"clips\": [", t > 0 ? "," : "",
                              oe_track_kind_get_name (track->kind));

      for (guint c = 0; c < track->clips->len; c++)
        {
          const OeClip *clip = g_ptr_array_index (track->clips, c);

          g_string_append (out, c > 0 ? ",\n        {\n" : "\n        {\n");
          g_string_append (out, "          \"media-ref\": ");
          append_int (out, clip->media_ref);
          g_string_append (out, ",\n          \"position-us\": ");
          append_int (out, clip->position_us);
          g_string_append (out, ",\n          \"source-in-us\": ");
          append_int (out, clip->source_in_us);
          g_string_append (out, ",\n          \"source-out-us\": ");
          append_int (out, clip->source_out_us);
          append_clip_visual (out, &clip->visual);
          append_clip_keyframes (out, &clip->visual);
          g_string_append (out, "\n        }");
        }
      if (track->clips->len > 0)
        g_string_append (out, "\n      ");
      g_string_append (out, "]");

      /* Track-level transitions (Phase 9 Wave B): written always, an
       * empty array when none; the sequence-level list is filtered to
       * this track. */
      g_string_append (out, ",\n        \"transitions\": [");

      guint track_transition_count = 0;

      for (guint tr = 0; tr < seq.transitions->len; tr++)
        {
          const OeTransition *transition = g_ptr_array_index (seq.transitions, tr);

          if (transition->track_index != t)
            continue;

          g_string_append (out, track_transition_count > 0 ? ",\n" : "\n");
          g_string_append (out, "          { \"at-us\": ");
          append_int (out, transition->at_us);
          g_string_append (out, ", \"duration-us\": ");
          append_int (out, transition->duration_us);
          g_string_append_printf (out, ", \"kind\": \"%s\" }",
                                  oe_transition_kind_get_name (transition->kind));
          track_transition_count++;
        }

      if (track_transition_count > 0)
        g_string_append (out, "\n        ");
      g_string_append (out, "]");

      g_string_append (out, " }");
    }
  if (seq.tracks->len > 0)
    g_string_append (out, "\n    ");
  g_string_append (out, "]\n");

  g_string_append (out, "  }\n}\n");

  oe_sequence_clear (&seq);
  return g_string_free (out, FALSE);
}

/* ------------------------------------------------------------------ */
/* Atomic write: temp in the target directory, fsync, rename.          */
/* ------------------------------------------------------------------ */

static gboolean
write_atomic (const gchar *path, const gchar *data, gsize len, GError **error)
{
  gchar *dir = g_path_get_dirname (path);
  gchar *tmp = g_strdup_printf ("%s/.obvious-edit-XXXXXX", dir);
  gboolean ok = TRUE;

  int fd = g_mkstemp (tmp);

  if (fd < 0)
    {
      int saved = errno;

      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_IO,
                   "cannot create a temp file for '%s': %s", path, g_strerror (saved));
      g_free (tmp);
      g_free (dir);
      return FALSE;
    }

  gsize written = 0;

  while (ok && written < len)
    {
      gssize n = write (fd, data + written, len - written);

      if (n < 0)
        {
          if (errno == EINTR)
            continue;

          int saved = errno;

          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_IO,
                       "cannot write '%s': %s", tmp, g_strerror (saved));
          ok = FALSE;
          break;
        }
      written += (gsize) n;
    }

  if (ok && fchmod (fd, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) != 0)
    {
      int saved = errno;

      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_IO,
                   "cannot set permissions on '%s': %s", tmp, g_strerror (saved));
      ok = FALSE;
    }

  if (ok && fsync (fd) != 0)
    {
      int saved = errno;

      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_IO,
                   "cannot flush '%s': %s", tmp, g_strerror (saved));
      ok = FALSE;
    }

  if (close (fd) != 0 && ok)
    {
      int saved = errno;

      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_IO,
                   "cannot close '%s': %s", tmp, g_strerror (saved));
      ok = FALSE;
    }

  if (ok && g_rename (tmp, path) != 0)
    {
      int saved = errno;

      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_IO,
                   "cannot move the project file into place at '%s': %s", path, g_strerror (saved));
      ok = FALSE;
    }

  if (!ok)
    g_unlink (tmp);

  g_free (tmp);
  g_free (dir);
  return ok;
}

gboolean
oe_project_format_save (OeProject *project, const gchar *path, GError **error)
{
  g_return_val_if_fail (OE_IS_PROJECT (project), FALSE);
  g_return_val_if_fail (path != NULL && path[0] != '\0', FALSE);

  gchar *data = serialize_project (project);
  gboolean ok = write_atomic (path, data, strlen (data), error);

  g_free (data);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Strict loading.                                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
  guint ref;
  gchar *path;
} MediaEntry;

typedef struct
{
  gint64 position_us;
  gint64 source_in_us;
  gint64 source_out_us;
  guint media_ref;
  OeClipVisual visual; /* identity when the file omitted the member */
} ClipEntry;

typedef struct
{
  OeTrackKind kind;
  GPtrArray *clips;    /* ClipEntry* */
  GArray *transitions; /* OeTransition values, file order */
} TrackEntry;

static ClipEntry *
clip_entry_new (gint64 position_us, gint64 source_in_us, gint64 source_out_us, guint media_ref,
                const OeClipVisual *visual)
{
  ClipEntry *clip = g_new0 (ClipEntry, 1);

  clip->position_us = position_us;
  clip->source_in_us = source_in_us;
  clip->source_out_us = source_out_us;
  clip->media_ref = media_ref;
  if (visual != NULL)
    clip->visual = *visual; /* Wave A visuals own no memory: value copy is deep */
  else
    clip->visual = oe_clip_visual_identity ();
  return clip;
}

/* Clip entries own memory now: the parsed visual may carry a
 * keyframe store. */
static void
clip_entry_free (gpointer data)
{
  ClipEntry *clip = data;

  oe_clip_visual_clear (&clip->visual);
  g_free (clip);
}

static TrackEntry *
track_entry_new (OeTrackKind kind)
{
  TrackEntry *track = g_new0 (TrackEntry, 1);

  track->kind = kind;
  track->clips = g_ptr_array_new_with_free_func (clip_entry_free);
  track->transitions = g_array_new (FALSE, FALSE, sizeof (OeTransition));
  return track;
}

static void
track_entry_free (gpointer data)
{
  TrackEntry *track = data;

  g_clear_pointer (&track->clips, g_ptr_array_unref);
  g_clear_pointer (&track->transitions, g_array_unref);
  g_free (track);
}

static void
media_entry_free (gpointer data)
{
  MediaEntry *entry = data;

  g_clear_pointer (&entry->path, g_free);
  g_free (entry);
}

/* Every object member must be one of @known — v1 is closed, because a
 * tolerated unknown member would be silently dropped on re-save. */
static gboolean
check_members (JsonObject *obj, const gchar *const *known, gsize n, const gchar *where,
               GError **error)
{
  /* json_object_get_members transfers the list container (the member
   * names stay owned by the object), so it must be freed on every path. */
  GList *members = json_object_get_members (obj);
  gboolean ok = TRUE;

  for (GList *l = members; l != NULL; l = l->next)
    {
      const gchar *name = l->data;
      gboolean known_name = FALSE;

      for (gsize i = 0; i < n; i++)
        {
          if (g_strcmp0 (name, known[i]) == 0)
            {
              known_name = TRUE;
              break;
            }
        }

      if (!known_name)
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_UNKNOWN_MEMBER,
                       "%s: unknown member \"%s\" — v1 is strict; the file may be from a "
                       "newer format version",
                       where, name);
          ok = FALSE;
          break;
        }
    }

  g_list_free (members);
  return ok;
}

static gboolean
require_node (JsonObject *obj, const gchar *name, JsonNodeType node_type, const gchar *where,
              JsonNode **out, GError **error)
{
  JsonNode *node = json_object_get_member (obj, name);

  if (node == NULL)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_MISSING,
                   "%s: missing required member \"%s\"", where, name);
      return FALSE;
    }

  if (JSON_NODE_TYPE (node) != node_type)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                   "%s: member \"%s\" has the wrong JSON type", where, name);
      return FALSE;
    }

  *out = node;
  return TRUE;
}

static gboolean
node_get_int (JsonNode *node, const gchar *where, const gchar *name, gint64 *out, GError **error)
{
  if (JSON_NODE_TYPE (node) != JSON_NODE_VALUE || json_node_get_value_type (node) != G_TYPE_INT64)
    {
      const gboolean is_double = JSON_NODE_TYPE (node) == JSON_NODE_VALUE
                                 && json_node_get_value_type (node) == G_TYPE_DOUBLE;

      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                   "%s: member \"%s\" must be an integer%s", where, name,
                   is_double ? " — floats are not allowed in v1" : "");
      return FALSE;
    }

  *out = json_node_get_int (node);
  return TRUE;
}

static gboolean
node_get_string (JsonNode *node, const gchar *where, const gchar *name, const gchar **out,
                 GError **error)
{
  if (JSON_NODE_TYPE (node) != JSON_NODE_VALUE || json_node_get_value_type (node) != G_TYPE_STRING)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                   "%s: member \"%s\" must be a string", where, name);
      return FALSE;
    }

  *out = json_node_get_string (node);
  return TRUE;
}

static gboolean
parse_media (JsonNode *node, GPtrArray *media_out, GError **error)
{
  JsonArray *array = json_node_get_array (node);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      gchar *where = g_strdup_printf ("media[%u]", i);
      JsonNode *entry_node = json_array_get_element (array, i);
      gboolean ok = FALSE;
      JsonObject *entry = NULL;
      JsonNode *ref_node = NULL;
      JsonNode *path_node = NULL;
      gint64 ref = 0;
      const gchar *path = NULL;

      if (!JSON_NODE_HOLDS_OBJECT (entry_node))
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                       "%s: must be an object", where);
          goto media_out;
        }

      entry = json_node_get_object (entry_node);

      static const gchar *const members[] = { "ref", "path" };

      if (!check_members (entry, members, G_N_ELEMENTS (members), where, error))
        goto media_out;

      if (!require_node (entry, "ref", JSON_NODE_VALUE, where, &ref_node, error)
          || !require_node (entry, "path", JSON_NODE_VALUE, where, &path_node, error))
        goto media_out;

      if (!node_get_int (ref_node, where, "ref", &ref, error))
        goto media_out;

      if (ref < 1 || ref > G_MAXUINT32)
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                       "%s: media ref %lld is out of range (must be 1..%u)", where, (long long) ref,
                       G_MAXUINT32);
          goto media_out;
        }

      if (!node_get_string (path_node, where, "path", &path, error) || path == NULL
          || path[0] == '\0')
        {
          if (error != NULL && *error == NULL)
            g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                         "%s: \"path\" must be a non-empty string", where);
          goto media_out;
        }

      for (guint j = 0; j < media_out->len; j++)
        {
          const MediaEntry *existing = g_ptr_array_index (media_out, j);

          if (existing->ref == (guint) ref)
            {
              g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                           "%s: duplicate media reference %u", where, (guint) ref);
              goto media_out;
            }
        }

      {
        MediaEntry *entry_val = g_new0 (MediaEntry, 1);

        entry_val->ref = (guint) ref;
        entry_val->path = g_strdup (path);
        g_ptr_array_add (media_out, entry_val);
      }

      ok = TRUE;

    media_out:
      g_free (where);
      if (!ok)
        return FALSE;
    }

  return TRUE;
}

/* The optional clip "visual" member (Phase 9 Wave A). Absent means
 * identity — the width/height backfill recipe applied per clip: no
 * version bump, older files load unchanged. Present means strict: a
 * closed member list, integer tokens only, and the same domain ranges
 * the validated mutator enforces. Wave A serializes no keyframe store,
 * so an owned-memory visual can never arrive here. */
static gboolean
parse_clip_visual (JsonNode *node, const gchar *where, OeClipVisual *visual, GError **error)
{
  static const gchar *const members[]
      = { "pos-x",  "pos-y",  "scale-permille", "rotation-cdeg", "opacity",    "crop-l",
          "crop-t", "crop-r", "crop-b",         "fade-in-us",    "fade-out-us" };

  static const struct
  {
    const gchar *name;
    gint64 min;
    gint64 max;
  } ranges[] = {
    { "pos-x", G_MININT64, G_MAXINT64 },
    { "pos-y", G_MININT64, G_MAXINT64 },
    { "scale-permille", 1, 32000 },
    { "rotation-cdeg", -36000, 36000 },
    { "opacity", 0, 255 },
    { "crop-l", 0, G_MAXINT },
    { "crop-t", 0, G_MAXINT },
    { "crop-r", 0, G_MAXINT },
    { "crop-b", 0, G_MAXINT },
    { "fade-in-us", 0, G_MAXINT64 },
    { "fade-out-us", 0, G_MAXINT64 },
  };

  if (!JSON_NODE_HOLDS_OBJECT (node))
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                   "%s.visual: must be an object", where);
      return FALSE;
    }

  JsonObject *obj = json_node_get_object (node);
  gchar *visual_where = g_strdup_printf ("%s.visual", where);

  if (!check_members (obj, members, G_N_ELEMENTS (members), visual_where, error))
    {
      g_free (visual_where);
      return FALSE;
    }

  *visual = oe_clip_visual_identity ();
  gint64 values[G_N_ELEMENTS (ranges)] = { 0 };

  for (guint i = 0; i < G_N_ELEMENTS (ranges); i++)
    {
      JsonNode *value_node = NULL;

      if (!require_node (obj, ranges[i].name, JSON_NODE_VALUE, visual_where, &value_node, error)
          || !node_get_int (value_node, visual_where, ranges[i].name, &values[i], error))
        {
          g_free (visual_where);
          return FALSE;
        }

      if (values[i] < ranges[i].min || values[i] > ranges[i].max)
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                       "%s: %s %lld is out of range (must be %lld..%lld)", visual_where,
                       ranges[i].name, (long long) values[i], (long long) ranges[i].min,
                       (long long) ranges[i].max);
          g_free (visual_where);
          return FALSE;
        }
    }

  g_free (visual_where);
  visual->pos_x = (gint) values[0];
  visual->pos_y = (gint) values[1];
  visual->scale_permille = (guint) values[2];
  visual->rotation_cdeg = (gint) values[3];
  visual->opacity = (guint8) values[4];
  visual->crop_l = (guint) values[5];
  visual->crop_t = (guint) values[6];
  visual->crop_r = (guint) values[7];
  visual->crop_b = (guint) values[8];
  visual->fade_in_us = (guint64) values[9];
  visual->fade_out_us = (guint64) values[10];
  return TRUE;
}

/* The optional clip "keyframes" member (Phase 9 Wave B). Absent
 * means none — their absence means none (the width/height backfill
 * recipe at clip level). Present means strict: an object whose closed
 * member set is the keyframeable property names; each value is an
 * array of {"time-us", "value"} objects — integer tokens only, times
 * non-negative, values inside the property's domain. Duplicate
 * (property, time) entries collapse through the sorted insert and the
 * store stays sorted on construction, so a hand-written unsorted or
 * duplicated array loads to exactly the bytes the writer emits. */
static gboolean
parse_clip_keyframes (JsonNode *node, const gchar *where, OeClipVisual *visual, GError **error)
{
  static const gchar *const members[]
      = { "opacity", "pos-x", "pos-y", "scale-permille", "rotation-cdeg" };
  static const gchar *const entry_members[] = { "time-us", "value" };

  if (!JSON_NODE_HOLDS_OBJECT (node))
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                   "%s.keyframes: must be an object", where);
      return FALSE;
    }

  JsonObject *obj = json_node_get_object (node);
  gchar *kf_where = g_strdup_printf ("%s.keyframes", where);
  GArray *store = g_array_new (FALSE, FALSE, sizeof (OeKeyframe));

  if (!check_members (obj, members, G_N_ELEMENTS (members), kf_where, error))
    goto fail;

  for (guint p = 0; p < G_N_ELEMENTS (members); p++)
    {
      JsonNode *prop_node = json_object_get_member (obj, members[p]);

      if (prop_node == NULL)
        continue;

      if (!JSON_NODE_HOLDS_ARRAY (prop_node))
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                       "%s: %s must be an array", kf_where, members[p]);
          goto fail;
        }

      OeKeyframeProperty property;

      g_assert (oe_keyframe_property_parse (members[p], &property));

      JsonArray *entries = json_node_get_array (prop_node);

      for (guint e = 0; e < json_array_get_length (entries); e++)
        {
          gchar *entry_where = g_strdup_printf ("%s: %s[%u]", kf_where, members[p], e);
          JsonNode *entry_node = json_array_get_element (entries, e);
          JsonObject *entry = NULL;
          JsonNode *time_node = NULL;
          JsonNode *value_node = NULL;
          gint64 time_us = 0;
          gint64 value = 0;

          if (!JSON_NODE_HOLDS_OBJECT (entry_node))
            {
              g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                           "%s: must be an object", entry_where);
              goto entry_fail;
            }

          entry = json_node_get_object (entry_node);

          if (!check_members (entry, entry_members, G_N_ELEMENTS (entry_members), entry_where,
                              error)
              || !require_node (entry, "time-us", JSON_NODE_VALUE, entry_where, &time_node, error)
              || !require_node (entry, "value", JSON_NODE_VALUE, entry_where, &value_node, error)
              || !node_get_int (time_node, entry_where, "time-us", &time_us, error)
              || !node_get_int (value_node, entry_where, "value", &value, error))
            goto entry_fail;

          if (time_us < 0 || value < G_MININT32 || value > G_MAXINT32
              || !oe_keyframe_value_in_domain (property, (gint32) value))
            {
              g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                           "%s: time-us %lld / value %lld is out of domain for %s", entry_where,
                           (long long) time_us, (long long) value, members[p]);
              goto entry_fail;
            }

          {
            OeKeyframe key = { property, time_us, (gint32) value };

            oe_keyframes_insert (store, key);
          }

          g_free (entry_where);
          continue;

        entry_fail:
          g_free (entry_where);
          goto fail;
        }
    }

  if (store->len > 0)
    visual->keyframes = store;
  else
    g_array_unref (store);

  g_free (kf_where);
  return TRUE;

fail:
  g_array_unref (store);
  g_free (kf_where);
  return FALSE;
}

static gboolean
parse_clip (JsonObject *obj, const gchar *where, const GPtrArray *media, ClipEntry **out,
            GError **error)
{
  static const gchar *const members[]
      = { "media-ref", "position-us", "source-in-us", "source-out-us", "visual", "keyframes" };

  JsonNode *media_ref_node = NULL;
  JsonNode *position_node = NULL;
  JsonNode *in_node = NULL;
  JsonNode *out_node = NULL;

  if (!check_members (obj, members, G_N_ELEMENTS (members), where, error))
    return FALSE;

  if (!require_node (obj, "media-ref", JSON_NODE_VALUE, where, &media_ref_node, error)
      || !require_node (obj, "position-us", JSON_NODE_VALUE, where, &position_node, error)
      || !require_node (obj, "source-in-us", JSON_NODE_VALUE, where, &in_node, error)
      || !require_node (obj, "source-out-us", JSON_NODE_VALUE, where, &out_node, error))
    return FALSE;

  gint64 media_ref = 0;
  gint64 position_us = 0;
  gint64 source_in_us = 0;
  gint64 source_out_us = 0;

  if (!node_get_int (media_ref_node, where, "media-ref", &media_ref, error)
      || !node_get_int (position_node, where, "position-us", &position_us, error)
      || !node_get_int (in_node, where, "source-in-us", &source_in_us, error)
      || !node_get_int (out_node, where, "source-out-us", &source_out_us, error))
    return FALSE;

  if (media_ref < 1 || media_ref > G_MAXUINT32)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                   "%s: media-ref %lld is out of range (must be 1..%u)", where,
                   (long long) media_ref, G_MAXUINT32);
      return FALSE;
    }

  if (position_us < 0)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                   "%s: position-us %lld is negative", where, (long long) position_us);
      return FALSE;
    }

  if (source_in_us < 0)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                   "%s: source-in-us %lld is negative", where, (long long) source_in_us);
      return FALSE;
    }

  if (source_out_us <= source_in_us)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                   "%s: source-out-us %lld must exceed source-in-us %lld (the clip duration is "
                   "source-out minus source-in; for stills the source range is the screen "
                   "duration)",
                   where, (long long) source_out_us, (long long) source_in_us);
      return FALSE;
    }

  gint64 duration = source_out_us - source_in_us;

  if (position_us > G_MAXINT64 - duration)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                   "%s: position-us %lld plus duration %lld overflows the timeline", where,
                   (long long) position_us, (long long) duration);
      return FALSE;
    }

  gboolean ref_known = FALSE;

  for (guint i = 0; i < media->len; i++)
    {
      const MediaEntry *entry = g_ptr_array_index (media, i);

      if (entry->ref == (guint) media_ref)
        {
          ref_known = TRUE;
          break;
        }
    }

  if (!ref_known)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                   "%s: media-ref %u names no entry in \"media\"", where, (guint) media_ref);
      return FALSE;
    }

  OeClipVisual visual = oe_clip_visual_identity ();

  JsonNode *visual_node = json_object_get_member (obj, "visual");

  if (visual_node != NULL && !parse_clip_visual (visual_node, where, &visual, error))
    return FALSE;

  JsonNode *keyframes_node = json_object_get_member (obj, "keyframes");

  if (keyframes_node != NULL && !parse_clip_keyframes (keyframes_node, where, &visual, error))
    return FALSE;

  *out = clip_entry_new (position_us, source_in_us, source_out_us, (guint) media_ref, &visual);
  return TRUE;
}

/* The optional track "transitions" member (Phase 9 Wave B). Absent
 * means none. Present means strict: an array of {"at-us",
 * "duration-us", "kind"} objects — integer tokens only, positive
 * times, a closed kind-name set. Boundary placement and coverage are
 * validated when the load applies each entry through the validated
 * mutator, exactly like a UI edit. */
static gboolean
parse_track_transitions (JsonNode *node, const gchar *where, GArray *transitions, GError **error)
{
  static const gchar *const members[] = { "at-us", "duration-us", "kind" };

  if (!JSON_NODE_HOLDS_ARRAY (node))
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                   "%s.transitions: must be an array", where);
      return FALSE;
    }

  JsonArray *array = json_node_get_array (node);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      gchar *t_where = g_strdup_printf ("%s.transitions[%u]", where, i);
      JsonNode *t_node = json_array_get_element (array, i);
      JsonObject *t_obj = NULL;
      JsonNode *at_node = NULL;
      JsonNode *dur_node = NULL;
      JsonNode *kind_node = NULL;
      gint64 at_us = 0;
      gint64 duration_us = 0;
      const gchar *kind = NULL;
      gboolean ok = FALSE;

      if (!JSON_NODE_HOLDS_OBJECT (t_node))
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                       "%s: must be an object", t_where);
          goto t_out;
        }

      t_obj = json_node_get_object (t_node);

      if (!check_members (t_obj, members, G_N_ELEMENTS (members), t_where, error)
          || !require_node (t_obj, "at-us", JSON_NODE_VALUE, t_where, &at_node, error)
          || !require_node (t_obj, "duration-us", JSON_NODE_VALUE, t_where, &dur_node, error)
          || !require_node (t_obj, "kind", JSON_NODE_VALUE, t_where, &kind_node, error)
          || !node_get_int (at_node, t_where, "at-us", &at_us, error)
          || !node_get_int (dur_node, t_where, "duration-us", &duration_us, error)
          || !node_get_string (kind_node, t_where, "kind", &kind, error))
        goto t_out;

      if (at_us <= 0 || duration_us <= 0)
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                       "%s: at-us %lld / duration-us %lld must both be positive", t_where,
                       (long long) at_us, (long long) duration_us);
          goto t_out;
        }

      {
        const gchar *names[] = {
          oe_transition_kind_get_name (OE_TRANSITION_CROSS_DISSOLVE),
          oe_transition_kind_get_name (OE_TRANSITION_DIP_TO_BLACK),
        };
        OeTransition stored = { 0, at_us, duration_us, OE_TRANSITION_CROSS_DISSOLVE };

        if (g_strcmp0 (kind, names[0]) == 0)
          stored.kind = OE_TRANSITION_CROSS_DISSOLVE;
        else if (g_strcmp0 (kind, names[1]) == 0)
          stored.kind = OE_TRANSITION_DIP_TO_BLACK;
        else
          {
            g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                         "%s: unknown kind \"%s\"", t_where, kind);
            goto t_out;
          }

        g_array_append_val (transitions, stored);
      }

      ok = TRUE;

    t_out:
      g_free (t_where);

      if (!ok)
        return FALSE;
    }

  return TRUE;
}

static gboolean
parse_tracks (JsonNode *node, const GPtrArray *media, GPtrArray *tracks_out, GError **error)
{
  JsonArray *array = json_node_get_array (node);

  for (guint t = 0; t < json_array_get_length (array); t++)
    {
      gchar *where = g_strdup_printf ("tracks[%u]", t);
      JsonNode *track_node = json_array_get_element (array, t);
      JsonObject *track = NULL;
      JsonNode *kind_node = NULL;
      JsonNode *clips_node = NULL;
      const gchar *kind = NULL;
      TrackEntry *track_entry = NULL;
      gboolean ok = FALSE;

      if (!JSON_NODE_HOLDS_OBJECT (track_node))
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                       "%s: must be an object", where);
          goto track_out;
        }

      track = json_node_get_object (track_node);

      static const gchar *const members[] = { "kind", "clips", "transitions" };

      if (!check_members (track, members, G_N_ELEMENTS (members), where, error))
        goto track_out;

      if (!require_node (track, "kind", JSON_NODE_VALUE, where, &kind_node, error)
          || !require_node (track, "clips", JSON_NODE_ARRAY, where, &clips_node, error))
        goto track_out;

      if (!node_get_string (kind_node, where, "kind", &kind, error))
        goto track_out;

      OeTrackKind track_kind;

      if (g_strcmp0 (kind, "video") == 0)
        track_kind = OE_TRACK_VIDEO;
      else if (g_strcmp0 (kind, "audio") == 0)
        track_kind = OE_TRACK_AUDIO;
      else
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                       "%s: unknown kind \"%s\" (expected \"video\" or \"audio\")", where, kind);
          goto track_out;
        }

      track_entry = track_entry_new (track_kind);

      JsonArray *clips = json_node_get_array (clips_node);

      for (guint c = 0; c < json_array_get_length (clips); c++)
        {
          gchar *clip_where = g_strdup_printf ("tracks[%u].clips[%u]", t, c);
          JsonNode *clip_node = json_array_get_element (clips, c);
          ClipEntry *clip = NULL;

          if (!JSON_NODE_HOLDS_OBJECT (clip_node))
            {
              g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                           "%s: must be an object", clip_where);
              g_free (clip_where);
              goto track_out;
            }

          if (!parse_clip (json_node_get_object (clip_node), clip_where, media, &clip, error))
            {
              g_free (clip_where);
              goto track_out;
            }

          g_ptr_array_add (track_entry->clips, clip);
          g_free (clip_where);
        }

      JsonNode *transitions_node = json_object_get_member (track, "transitions");

      if (transitions_node != NULL
          && !parse_track_transitions (transitions_node, where, track_entry->transitions, error))
        goto track_out;

      g_ptr_array_add (tracks_out, track_entry);
      track_entry = NULL;
      ok = TRUE;

    track_out:
      g_free (where);

      /* track_entry stays owned by tracks_out on success; free the
       * local only when parsing failed mid-track. */
      if (track_entry != NULL && !ok)
        track_entry_free (track_entry);

      if (!ok)
        return FALSE;
    }

  return TRUE;
}

OeProject *
oe_project_format_load (const gchar *path, GError **error)
{
  g_return_val_if_fail (path != NULL && path[0] != '\0', NULL);

  gchar *data = NULL;
  gsize len = 0;
  GError *io_error = NULL;

  if (!g_file_get_contents (path, &data, &len, &io_error))
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_IO,
                   "cannot read '%s': %s", path, io_error != NULL ? io_error->message : "unknown");
      g_clear_error (&io_error);
      return NULL;
    }

  JsonParser *parser = json_parser_new ();

  if (!json_parser_load_from_data (parser, data, (gssize) len, &io_error))
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_SYNTAX,
                   "'%s' is not valid JSON: %s", path,
                   io_error != NULL ? io_error->message : "unknown");
      g_clear_error (&io_error);
      g_object_unref (parser);
      g_free (data);
      return NULL;
    }

  OeProject *project = NULL;
  GPtrArray *media = g_ptr_array_new_with_free_func (media_entry_free);
  GPtrArray *tracks = g_ptr_array_new_with_free_func (track_entry_free);
  OeRational rate = { 0, 0 };
  gchar *name = NULL;

  JsonNode *root = json_parser_get_root (parser);

  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_SYNTAX,
                   "'%s': the document root must be a JSON object", path);
      goto out;
    }

  JsonObject *root_obj;

  root_obj = json_node_get_object (root);

  JsonNode *doc_node;

  doc_node = json_object_get_member (root_obj, ROOT_KEY);

  if (doc_node == NULL)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_MISSING,
                   "'%s': missing required member \"%s\" at the document root", path, ROOT_KEY);
      goto out;
    }

  if (json_object_get_size (root_obj) != 1)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_UNKNOWN_MEMBER,
                   "'%s': the root may hold only \"%s\"", path, ROOT_KEY);
      goto out;
    }

  if (!JSON_NODE_HOLDS_OBJECT (doc_node))
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_TYPE,
                   "'%s': \"%s\" must be an object", path, ROOT_KEY);
      goto out;
    }

  JsonObject *doc;

  doc = json_node_get_object (doc_node);

  static const gchar *const doc_members[]
      = { "format-version", "name", "frame-rate", "width", "height", "media", "tracks" };

  if (!check_members (doc, doc_members, G_N_ELEMENTS (doc_members), ROOT_KEY, error))
    goto out;

  JsonNode *version_node;

  if (json_object_get_member (doc, "format-version") == NULL)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_MISSING,
                   "%s: missing required member \"format-version\"", ROOT_KEY);
      goto out;
    }

  version_node = json_object_get_member (doc, "format-version");

  gint64 version;

  if (!node_get_int (version_node, ROOT_KEY, "format-version", &version, error))
    goto out;

  if (version > OE_PROJECT_FORMAT_VERSION)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VERSION,
                   "format-version %lld is newer than this build supports (%d) — the file was "
                   "written by a newer Obvious Edit",
                   (long long) version, OE_PROJECT_FORMAT_VERSION);
      goto out;
    }

  if (version != OE_PROJECT_FORMAT_VERSION)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                   "format-version must be %d (got %lld)", OE_PROJECT_FORMAT_VERSION,
                   (long long) version);
      goto out;
    }

  JsonNode *name_node;
  JsonNode *rate_node;
  JsonNode *media_node;
  JsonNode *tracks_node;

  if (!require_node (doc, "name", JSON_NODE_VALUE, ROOT_KEY, &name_node, error)
      || !require_node (doc, "frame-rate", JSON_NODE_OBJECT, ROOT_KEY, &rate_node, error)
      || !require_node (doc, "media", JSON_NODE_ARRAY, ROOT_KEY, &media_node, error)
      || !require_node (doc, "tracks", JSON_NODE_ARRAY, ROOT_KEY, &tracks_node, error))
    goto out;

  const gchar *name_view;

  if (!node_get_string (name_node, ROOT_KEY, "name", &name_view, error))
    goto out;

  name = g_strdup (name_view);

  JsonObject *rate_obj;

  rate_obj = json_node_get_object (rate_node);

  static const gchar *const rate_members[] = { "num", "den" };

  if (!check_members (rate_obj, rate_members, G_N_ELEMENTS (rate_members), "frame-rate", error))
    goto out;

  JsonNode *num_node;
  JsonNode *den_node;

  if (!require_node (rate_obj, "num", JSON_NODE_VALUE, "frame-rate", &num_node, error)
      || !require_node (rate_obj, "den", JSON_NODE_VALUE, "frame-rate", &den_node, error))
    goto out;

  gint64 num = 0;
  gint64 den = 0;

  if (!node_get_int (num_node, "frame-rate", "num", &num, error)
      || !node_get_int (den_node, "frame-rate", "den", &den, error))
    goto out;

  GError *rate_error = NULL;

  rate = oe_time_rate (num, den, &rate_error);

  if (rate_error != NULL)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE, "frame-rate: %s",
                   rate_error->message);
      g_error_free (rate_error);
      goto out;
    }

  /* Optional sequence size (Phase 8): written always since the fields
   * exist, tolerated absent for files written before them — older
   * documents backfill to the defaults. Present values must be
   * positive even integers (yuv420p export compatibility). */
  gint width = OE_SEQUENCE_DEFAULT_WIDTH;
  gint height = OE_SEQUENCE_DEFAULT_HEIGHT;
  JsonNode *size_node;

  size_node = json_object_get_member (doc, "width");
  if (size_node != NULL)
    {
      gint64 value = 0;

      if (!node_get_int (size_node, ROOT_KEY, "width", &value, error))
        goto out;
      if (value <= 0 || value % 2 != 0 || value > G_MAXINT32)
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                       "width: must be a positive even integer");
          goto out;
        }
      width = (gint) value;
    }

  size_node = json_object_get_member (doc, "height");
  if (size_node != NULL)
    {
      gint64 value = 0;

      if (!node_get_int (size_node, ROOT_KEY, "height", &value, error))
        goto out;
      if (value <= 0 || value % 2 != 0 || value > G_MAXINT32)
        {
          g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                       "height: must be a positive even integer");
          goto out;
        }
      height = (gint) value;
    }

  if (!parse_media (media_node, media, error))
    goto out;

  if (!parse_tracks (tracks_node, media, tracks, error))
    goto out;

  /* The document is fully validated — construct the model. Any failure
   * below is a defensive guard, never an expected path. */
  project = oe_project_new (rate);

  if (project == NULL)
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE,
                   "internal: frame rate %lld/%lld was rejected after validation",
                   (long long) rate.num, (long long) rate.den);
      goto out;
    }

  oe_project_set_name (project, name);

  /* Pre-validated above; failure here is a defensive guard. */
  GError *size_error = NULL;

  if (!oe_project_set_sequence_size (project, width, height, &size_error))
    {
      g_set_error (error, OE_PROJECT_FORMAT_ERROR, OE_PROJECT_FORMAT_ERROR_VALUE, "size: %s",
                   size_error->message);
      g_error_free (size_error);
      g_clear_object (&project);
      goto out;
    }

  for (guint i = 0; i < media->len; i++)
    {
      const MediaEntry *entry = g_ptr_array_index (media, i);
      GError *add_error = NULL;

      if (!oe_project_add_media_ref (project, entry->ref, entry->path, &add_error))
        {
          g_propagate_error (error, add_error);
          g_clear_object (&project);
          goto out;
        }
    }

  for (guint t = 0; t < tracks->len; t++)
    {
      const TrackEntry *track = g_ptr_array_index (tracks, t);
      guint track_index = oe_project_add_track (project, track->kind);

      for (guint c = 0; c < track->clips->len; c++)
        {
          const ClipEntry *clip = g_ptr_array_index (track->clips, c);
          GError *insert_error = NULL;

          if (!oe_project_insert_clip (project, track_index, clip->media_ref, clip->position_us,
                                       clip->source_in_us, clip->source_out_us, &insert_error))
            {
              g_propagate_error (error, insert_error);
              g_clear_object (&project);
              goto out;
            }

          /* Only non-default visuals touch the mutator: the insert
           * already produced the identity state, and the load path stays
           * free of per-clip observer noise. */
          if (!oe_clip_visual_is_default (&clip->visual))
            {
              GError *visual_error = NULL;

              if (!oe_project_set_clip_visual (project, track_index, c, &clip->visual,
                                               &visual_error))
                {
                  g_propagate_error (error, visual_error);
                  g_clear_object (&project);
                  goto out;
                }
            }
        }

      /* Transitions bind to their owning track and apply through the
       * validated mutator: a file naming an empty boundary or an
       * uncovered window fails the load with the same typed error a
       * UI edit would produce, and stored durations re-clamp to the
       * same coverage the writer saw. */
      for (guint i = 0; i < track->transitions->len; i++)
        {
          OeTransition stored = g_array_index (track->transitions, OeTransition, i);
          GError *trans_error = NULL;

          stored.track_index = track_index;

          if (!oe_project_add_transition (project, &stored, &trans_error))
            {
              g_propagate_error (error, trans_error);
              g_clear_object (&project);
              goto out;
            }
        }
    }

out:
  g_ptr_array_unref (media);
  g_ptr_array_unref (tracks);
  g_free (name);
  g_object_unref (parser);
  g_free (data);
  return project;
}
