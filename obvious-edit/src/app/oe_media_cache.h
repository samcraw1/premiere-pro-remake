/* oe_media_cache.h — raw-binary thumbnail/waveform cache (Phase 2).
 *
 * Entries live under $XDG_CACHE_HOME/obvious-edit/media/ (override with
 * OE_MEDIA_CACHE_DIR for tests), keyed by a checksum of the canonical
 * path plus the file's size and mtime, so a changed file maps to a new
 * key and stale entries are simply never read again. There is no
 * eviction policy; the directory is safe to delete at any time and a
 * corrupt or unreadable entry is treated as a miss and regenerated.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * oe_media_cache_key_for_file:
 * @path: media file path (need not be canonical)
 * @error: return location for a #GError, or NULL to ignore
 *
 * Computes the cache key for @path: SHA-256 over the canonicalised path
 * plus the file's size and mtime at cache-hit time, hex-encoded. The
 * key is stable while the file content is untouched.
 *
 * Returns: (transfer full): newly allocated key string, or NULL with
 * @error set when the file cannot be stat'ed (e.g. already deleted).
 */
gchar *oe_media_cache_key_for_file (const gchar *path, GError **error);

/**
 * oe_media_cache_path_for_key:
 * @key: cache key from oe_media_cache_key_for_file()
 *
 * Returns: (transfer full): filesystem path of the entry for @key.
 * Exposed so callers can observe (not manage) the layout; the key files
 * are read and written only through this module.
 */
gchar *oe_media_cache_path_for_key (const gchar *key);

/**
 * oe_media_cache_lookup:
 * @key: cache key from oe_media_cache_key_for_file()
 * @out_data: (transfer full): receives the stored bytes on a hit
 * @out_len: receives the byte count on a hit
 *
 * Reads a cache entry. A miss (no file) or a corrupt entry returns
 * FALSE without setting @error — both mean "regenerate".
 *
 * Returns: TRUE on a hit with @out_data/@out_len set (caller frees).
 */
gboolean oe_media_cache_lookup (const gchar *key, guchar **out_data, gsize *out_len);

/**
 * oe_media_cache_store:
 * @key: cache key from oe_media_cache_key_for_file()
 * @data: bytes to store
 * @len: byte count
 * @error: return location for a #GError, or NULL to ignore
 *
 * Writes a cache entry atomically (temp file + rename), creating the
 * cache directory on demand. Storage is best-effort by design: a write
 * failure leaves the cache cold but returns FALSE with @error set so
 * callers can log it.
 *
 * Returns: TRUE on success.
 */
gboolean oe_media_cache_store (const gchar *key, gconstpointer data, gsize len, GError **error);

G_END_DECLS
