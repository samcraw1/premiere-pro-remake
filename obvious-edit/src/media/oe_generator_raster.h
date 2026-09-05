/* oe_generator_raster.h — generated-clip rasterization (Phase 11
 * Wave A).
 *
 * Titles and solids produce BGRA straight-alpha buffers at sequence
 * resolution (D12/D13): rasterized once per (clip identity, text,
 * size, color), cached by the render session, and consumed by both
 * the monitor and the export path — never re-rendered per frame and
 * never re-rendered per canvas size. The monitor downscales the same
 * buffer with the integer nearest-neighbor scale (generators are
 * SCALED, never box-fitted).
 *
 * The pinned reference family is "DejaVu Sans" (spec D12): the toy
 * API string, not a vendored font binary; tests assert presence and
 * coverage, never glyph shapes. GTK-free by design.
 */

#pragma once

#include <glib.h>

#include "../core/oe_project.h"

G_BEGIN_DECLS

/**
 * OeGeneratorRasterError: rasterization failure domain.
 */
typedef enum
{
  OE_GENERATOR_RASTER_ERROR_FAILED,
} OeGeneratorRasterError;

#define OE_GENERATOR_RASTER_ERROR (oe_generator_raster_error_quark ())

GQuark oe_generator_raster_error_quark (void);

/**
 * OeGeneratorCache: render-session-owned raster cache (the R3
 * mitigation). The session drops the whole cache on sequence snapshot
 * refresh, so an edit followed by the paused-repaint force-render
 * shows fresh text.
 */
typedef struct _OeGeneratorCache OeGeneratorCache;

OeGeneratorCache *oe_generator_cache_new (void);
void oe_generator_cache_free (OeGeneratorCache *cache);

/**
 * oe_generator_cache_raster: the cached sequence-resolution raster of
 * @clip's generated payload at @width x @height.
 *
 * Keyed by (clip pointer identity, text, color, size) — the model is
 * immutable between snapshot refreshes, so a hit reuses the buffer
 * unchanged. The returned pixels are owned by the cache and stay
 * valid until the cache is freed.
 */
const guint8 *oe_generator_cache_raster (OeGeneratorCache *cache, const OeClip *clip, int width,
                                         int height, GError **error);

/**
 * oe_generator_raster_title: fresh BGRA straight-alpha buffer with
 * @text centered in the frame (D10), ink height @size_permille
 * permille of @height, @color_rgb packed 0xRRGGBB. Cairo ARGB32 is
 * premultiplied, so the output is un-premultiplied once, pixel-exact
 * integer math. Deterministic for identical inputs.
 */
guint8 *oe_generator_raster_title (const gchar *text, gint color_rgb, gint size_permille, int width,
                                   int height, GError **error);

/**
 * oe_generator_raster_solid: fresh BGRA straight-alpha buffer filled
 * with @color_rgb at full opacity, at layer size.
 */
guint8 *oe_generator_raster_solid (gint color_rgb, int width, int height);

G_END_DECLS
