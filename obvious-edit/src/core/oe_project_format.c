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

static gchar *
serialize_project (OeProject *project)
{
  GString *out = g_string_sized_new (4096);

  OeSequence seq;

  oe_project_get_sequence (project, &seq);
  const gchar *name = oe_project_get_name (project);
  guint media_count = oe_project_get_media_count (project);

  g_string_append (out, "{\n  \"" ROOT_KEY "\": {\n");

  /* Version first, then name, frame-rate, media, tracks — the schema
   * order. Integers only, rates as num/den pairs. */
  g_string_append_printf (out, "    \"format-version\": %d,\n", OE_PROJECT_FORMAT_VERSION);

  g_string_append (out, "    \"name\": ");
  append_quoted (out, name != NULL ? name : "");
  g_string_append (out, ",\n");

  g_string_append (out, "    \"frame-rate\": { \"num\": ");
  append_int (out, seq.frame_rate.num);
  g_string_append (out, ", \"den\": ");
  append_int (out, seq.frame_rate.den);
  g_string_append (out, " },\n");

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
          g_string_append (out, "\n        }");
        }
      if (track->clips->len > 0)
        g_string_append (out, "\n      ");
      g_string_append (out, "] }");
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
} ClipEntry;

typedef struct
{
  OeTrackKind kind;
  GPtrArray *clips; /* ClipEntry* */
} TrackEntry;

static ClipEntry *
clip_entry_new (gint64 position_us, gint64 source_in_us, gint64 source_out_us, guint media_ref)
{
  ClipEntry *clip = g_new0 (ClipEntry, 1);

  clip->position_us = position_us;
  clip->source_in_us = source_in_us;
  clip->source_out_us = source_out_us;
  clip->media_ref = media_ref;
  return clip;
}

static TrackEntry *
track_entry_new (OeTrackKind kind)
{
  TrackEntry *track = g_new0 (TrackEntry, 1);

  track->kind = kind;
  track->clips = g_ptr_array_new_with_free_func (g_free);
  return track;
}

static void
track_entry_free (gpointer data)
{
  TrackEntry *track = data;

  g_clear_pointer (&track->clips, g_ptr_array_unref);
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
  GList *members = json_object_get_members (obj);

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
          return FALSE;
        }
    }
  return TRUE;
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

static gboolean
parse_clip (JsonObject *obj, const gchar *where, const GPtrArray *media, ClipEntry **out,
            GError **error)
{
  static const gchar *const members[]
      = { "media-ref", "position-us", "source-in-us", "source-out-us" };

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

  *out = clip_entry_new (position_us, source_in_us, source_out_us, (guint) media_ref);
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

      static const gchar *const members[] = { "kind", "clips" };

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
      = { "format-version", "name", "frame-rate", "media", "tracks" };

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
