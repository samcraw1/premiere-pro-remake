/* oe_media_cache.c — raw-binary thumbnail/waveform cache implementation.
 *
 * Safe to call from the import worker thread: every entry write is a
 * temp-file rename and every read tolerates absence or corruption.
 */

#include "oe_media_cache.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "oe_log.h"

/* Test-isolation override; production uses the XDG cache directory. */
#define OE_MEDIA_CACHE_DIR_ENV "OE_MEDIA_CACHE_DIR"

/* Entry framing: magic + little-endian payload length + payload bytes.
 * Anything that does not parse exactly is a miss, so torn, truncated, or
 * hand-corrupted files never surface as media data. */
static const gchar ENTRY_MAGIC[4] = { 'O', 'E', '0', '1' };

#define ENTRY_HEADER_SIZE 8

gchar *
oe_media_cache_key_for_file (const gchar *path, GError **error)
{
  g_return_val_if_fail (path != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  /* Canonicalise so two spellings of the same file share one entry. */
  gchar *canonical = g_canonicalize_filename (path, NULL);

  if (canonical == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "cannot canonicalise '%s'", path);
      return NULL;
    }

  GStatBuf st = { 0 };

  if (g_stat (canonical, &st) != 0)
    {
      int saved = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved), "cannot stat '%s': %s",
                   canonical, g_strerror (saved));
      g_free (canonical);
      return NULL;
    }

  GChecksum *sum = g_checksum_new (G_CHECKSUM_SHA256);

  g_checksum_update (sum, (const guchar *) canonical, (gssize) strlen (canonical));
  g_checksum_update (sum, (const guchar *) "|", 1);

  gchar meta[G_ASCII_DTOSTR_BUF_SIZE];
  gint64 size = (gint64) st.st_size;
  gint64 mtime = (gint64) st.st_mtime;

  g_snprintf (meta, sizeof (meta), "%lld|%lld", (long long) size, (long long) mtime);
  g_checksum_update (sum, (const guchar *) meta, (gssize) strlen (meta));

  const gchar *hex = g_checksum_get_string (sum);

  /* Ownership moves out; the checksum string is owned by the GChecksum. */
  gchar *key = g_strdup (hex);

  g_checksum_free (sum);
  g_free (canonical);
  return key;
}

static gchar *
cache_base_dir (void)
{
  const gchar *override = g_getenv (OE_MEDIA_CACHE_DIR_ENV);

  if (override != NULL && override[0] != '\0')
    return g_strdup (override);

  return g_build_filename (g_get_user_cache_dir (), "obvious-edit", "media", NULL);
}

gchar *
oe_media_cache_path_for_key (const gchar *key)
{
  g_return_val_if_fail (key != NULL, NULL);

  gchar *base = cache_base_dir ();
  gchar *full = g_build_filename (base, key, NULL);

  g_free (base);
  return full;
}

gboolean
oe_media_cache_lookup (const gchar *key, guchar **out_data, gsize *out_len)
{
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (out_data != NULL, FALSE);
  g_return_val_if_fail (out_len != NULL, FALSE);

  *out_data = NULL;
  *out_len = 0;

  gchar *entry_path = oe_media_cache_path_for_key (key);
  gsize len = 0;
  gchar *contents = NULL;
  gboolean ok = g_file_get_contents (entry_path, &contents, &len, NULL);

  g_free (entry_path);

  if (!ok)
    return FALSE; /* absent entry: a miss, not an error */

  if (len < ENTRY_HEADER_SIZE || memcmp (contents, ENTRY_MAGIC, sizeof ENTRY_MAGIC) != 0)
    {
      g_free (contents);
      return FALSE; /* corrupt entry: a miss, not an error */
    }

  guint32 stored_len = 0;

  memcpy (&stored_len, contents + 4, sizeof stored_len);
  stored_len = GUINT32_FROM_LE (stored_len);

  if (stored_len != (guint32) (len - ENTRY_HEADER_SIZE))
    {
      g_free (contents);
      return FALSE; /* truncated or padded entry: also a miss */
    }

  *out_len = len - ENTRY_HEADER_SIZE;
  *out_data = g_memdup2 (contents + ENTRY_HEADER_SIZE, *out_len);
  g_free (contents);
  return TRUE;
}

gboolean
oe_media_cache_store (const gchar *key, gconstpointer data, gsize len, GError **error)
{
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (data != NULL || len == 0, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  gchar *base = cache_base_dir ();
  int rv = g_mkdir_with_parents (base, 0700);

  if (rv != 0)
    {
      int saved = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved),
                   "cannot create cache dir '%s': %s", base, g_strerror (saved));
      g_free (base);
      return FALSE;
    }

  /* Temp file in the same directory, then rename: readers either see
   * the previous state or the complete new entry, never a torn write. */
  gchar *entry_path = g_build_filename (base, key, NULL);
  gchar *tmp_path = g_strdup_printf ("%s.tmp.%d", entry_path, (int) getpid ());

  /* Frame the payload before writing so readers can reject corruption. */
  guchar *framed = g_malloc (ENTRY_HEADER_SIZE + len);

  memcpy (framed, ENTRY_MAGIC, sizeof ENTRY_MAGIC);
  guint32 payload_len = GUINT32_TO_LE ((guint32) len);

  memcpy (framed + 4, &payload_len, sizeof payload_len);
  if (len > 0)
    memcpy (framed + ENTRY_HEADER_SIZE, data, len);

  gboolean ok = g_file_set_contents (tmp_path, (const gchar *) framed,
                                     (gssize) (ENTRY_HEADER_SIZE + len), error);

  g_free (framed);

  if (ok && g_rename (tmp_path, entry_path) != 0)
    {
      int saved = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved),
                   "cannot move '%s' into place: %s", entry_path, g_strerror (saved));
      ok = FALSE;
    }

  if (!ok)
    g_unlink (tmp_path);

  g_free (tmp_path);
  g_free (entry_path);
  g_free (base);
  return ok;
}
