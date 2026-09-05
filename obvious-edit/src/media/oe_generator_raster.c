/* oe_generator_raster.c — generated-clip rasterization (Phase 11
 * Wave A).
 *
 * Titles use the Cairo toy font API with the pinned reference family
 * "DejaVu Sans" (spec D12): rasterized once at sequence resolution,
 * centered ink (D10), then un-premultiplied into the pipeline's
 * straight-alpha BGRA convention. Solids fill directly at layer
 * size. All math outside Cairo is integer and deterministic.
 */

#include "oe_generator_raster.h"

#include <string.h>

#include <cairo.h>

/* ------------------------------------------------------------------ */
/* Error domain                                                        */
/* ------------------------------------------------------------------ */

GQuark
oe_generator_raster_error_quark (void)
{
  static GQuark quark = 0;

  if (quark == 0)
    quark = g_quark_from_static_string ("oe-generator-raster-error");
  return quark;
}

/* ------------------------------------------------------------------ */
/* Rasterizers                                                         */
/* ------------------------------------------------------------------ */

/* Un-premultiplies a Cairo ARGB32 row block in place into the
 * straight-alpha convention the compositor blends with: c = round
 * (premul * 255 / a) for a > 0, exact integer round-half-up. Cairo
 * orders ARGB32 bytes B,G,R,A on little-endian, matching the
 * pipeline's BGRA layout — only the alpha math needs conversion. */
static void
unpremultiply_bgra (guint8 *pixels, gsize pixel_count)
{
  for (gsize i = 0; i < pixel_count; i++)
    {
      guint8 *p = pixels + i * 4;
      const guint a = p[3];

      if (a == 255 || a == 0)
        continue;

      p[0] = (guint8) ((p[0] * 255 + a / 2) / a);
      p[1] = (guint8) ((p[1] * 255 + a / 2) / a);
      p[2] = (guint8) ((p[2] * 255 + a / 2) / a);
    }
}

guint8 *
oe_generator_raster_title (const gchar *text, gint color_rgb, gint size_permille, int width,
                           int height, GError **error)
{
  g_return_val_if_fail (text != NULL && text[0] != '\0', NULL);
  g_return_val_if_fail (width > 0 && height > 0, NULL);

  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create (surface);

  /* The pinned reference family (D12): one slant, one weight, no
   * style fields — D1's single fixed style. */
  cairo_select_font_face (cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

  /* Ink height as permille of frame height: the toy font size in user
   * units maps 1:1 to surface pixels at the identity CTM. */
  cairo_set_font_size (cr, (double) height * (double) size_permille / 1000.0);
  cairo_set_source_rgb (cr, (double) ((color_rgb >> 16) & 0xff) / 255.0,
                        (double) ((color_rgb >> 8) & 0xff) / 255.0,
                        (double) (color_rgb & 0xff) / 255.0);

  /* Centered ink (D10): offset the baseline so the ink box centers in
   * the frame regardless of bearing. */
  cairo_text_extents_t ext;

  cairo_text_extents (cr, text, &ext);
  const double pen_x = (double) (width - ext.width) / 2.0 - ext.x_bearing;
  const double pen_y = (double) (height - ext.height) / 2.0 - ext.y_bearing;

  cairo_move_to (cr, pen_x, pen_y);
  cairo_show_text (cr, text);

  guint8 *out = NULL;

  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS
      || cairo_status (cr) != CAIRO_STATUS_SUCCESS)
    {
      g_set_error (error, OE_GENERATOR_RASTER_ERROR, OE_GENERATOR_RASTER_ERROR_FAILED,
                   "title rasterization failed: %s",
                   cairo_status_to_string (cairo_status (cr) != CAIRO_STATUS_SUCCESS
                                               ? cairo_status (cr)
                                               : cairo_surface_status (surface)));
    }
  else
    {
      cairo_surface_flush (surface);

      const gsize pixel_count = (gsize) width * (gsize) height;

      out = g_memdup2 (cairo_image_surface_get_data (surface), pixel_count * 4);
      unpremultiply_bgra (out, pixel_count);
    }

  cairo_destroy (cr);
  cairo_surface_destroy (surface);
  return out;
}

guint8 *
oe_generator_raster_solid (gint color_rgb, int width, int height)
{
  g_return_val_if_fail (width > 0 && height > 0, NULL);

  const gsize pixel_count = (gsize) width * (gsize) height;
  guint8 *out = g_malloc (pixel_count * 4);

  for (gsize i = 0; i < pixel_count; i++)
    {
      out[i * 4 + 0] = (guint8) (color_rgb & 0xff);         /* B */
      out[i * 4 + 1] = (guint8) ((color_rgb >> 8) & 0xff);  /* G */
      out[i * 4 + 2] = (guint8) ((color_rgb >> 16) & 0xff); /* R */
      out[i * 4 + 3] = 255;
    }

  return out;
}

/* ------------------------------------------------------------------ */
/* Session-owned cache                                                 */
/* ------------------------------------------------------------------ */

struct _OeGeneratorCache
{
  /* Owned RasterKey → owned RasterEntry. */
  GHashTable *rasters;
};

typedef struct
{
  const OeClip *clip; /* model identity: immutable between refreshes */
  gchar *text;        /* owned */
  gint color_rgb;
  gint size_permille;
  gint width;
  gint height;
} RasterKey;

typedef struct
{
  guint8 *pixels; /* owned: width*height*4 BGRA straight alpha */
} RasterEntry;

static guint
raster_key_hash (gconstpointer v)
{
  const RasterKey *k = v;

  return (guint) (g_direct_hash (k->clip) ^ g_str_hash (k->text)
                  ^ ((guint) k->color_rgb * 31u + (guint) k->size_permille * 7u
                     + (guint) k->width * 3u + (guint) k->height));
}

static gboolean
raster_key_equal (gconstpointer a, gconstpointer b)
{
  const RasterKey *ka = a;
  const RasterKey *kb = b;

  return ka->clip == kb->clip && g_strcmp0 (ka->text, kb->text) == 0
         && ka->color_rgb == kb->color_rgb && ka->size_permille == kb->size_permille
         && ka->width == kb->width && ka->height == kb->height;
}

static void
raster_key_free (gpointer data)
{
  RasterKey *k = data;

  g_free (k->text);
  g_free (k);
}

static void
raster_entry_free (gpointer data)
{
  RasterEntry *e = data;

  g_free (e->pixels);
  g_free (e);
}

OeGeneratorCache *
oe_generator_cache_new (void)
{
  OeGeneratorCache *cache = g_new0 (OeGeneratorCache, 1);

  cache->rasters = g_hash_table_new_full (raster_key_hash, raster_key_equal, raster_key_free,
                                          raster_entry_free);
  return cache;
}

void
oe_generator_cache_free (OeGeneratorCache *cache)
{
  if (cache == NULL)
    return;

  g_clear_pointer (&cache->rasters, g_hash_table_unref);
  g_free (cache);
}

const guint8 *
oe_generator_cache_raster (OeGeneratorCache *cache, const OeClip *clip, int width, int height,
                           GError **error)
{
  g_return_val_if_fail (cache != NULL, NULL);
  g_return_val_if_fail (clip != NULL, NULL);
  g_return_val_if_fail (width > 0 && height > 0, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  const OeClipGenerator *payload = &clip->generator;
  RasterKey *key = g_new0 (RasterKey, 1);

  key->clip = clip;
  key->text = g_strdup (payload->text != NULL ? payload->text : "");
  key->color_rgb = payload->color_rgb;
  key->size_permille = payload->size_permille;
  key->width = width;
  key->height = height;

  RasterEntry *hit = g_hash_table_lookup (cache->rasters, key);

  if (hit != NULL)
    {
      raster_key_free (key);
      return hit->pixels;
    }

  guint8 *pixels = NULL;

  if (clip->kind == OE_CLIP_TITLE)
    pixels
        = oe_generator_raster_title (payload->text != NULL ? payload->text : "", payload->color_rgb,
                                     payload->size_permille, width, height, error);
  else
    pixels = oe_generator_raster_solid (payload->color_rgb, width, height);

  if (pixels == NULL)
    {
      raster_key_free (key);
      return NULL;
    }

  RasterEntry *entry = g_new0 (RasterEntry, 1);

  entry->pixels = pixels;
  g_hash_table_replace (cache->rasters, key, entry); /* cache owns both */
  return pixels;
}
