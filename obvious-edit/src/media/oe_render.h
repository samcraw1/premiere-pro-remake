/* oe_render.h — GTK-free frame-at-time render seam (Phase 8).
 *
 * The one seam that answers "what does the sequence look like at
 * sequence time t?": it maps t through the model's clip→source
 * mapping (oe_playback_session_map) and composites the covering
 * clip's frame into an owned B8G8R8A8 canvas. Export and any future
 * preview-side consumer share this path, so a straight cut renders
 * identically everywhere.
 *
 * Compositing semantics (locked decision D2): the topmost video
 * track's covering clip wins, the source time is source_in +
 * (position - clip_start), coverage is half-open. A frame with no
 * covering clip renders opaque black. The decoded frame is box-fitted
 * into the requested canvas, aspect preserved, centered — the same
 * presentation rule the program monitor applies.
 *
 * Decoder cost control (D2): a one-shot call opens and caches one
 * decoder per source path inside a session; the session's frame_at
 * decodes strictly forward for increasing times (the export loop never
 * seeks backward per frame). Backward requests fall back to the
 * decode-at seek discipline.
 *
 * Threading: a session is single-threaded — create, use, and free it
 * on one thread (the export worker). GTK-free: GLib + FFmpeg only.
 */

#pragma once

#include <glib.h>

#include "../core/oe_project.h"

G_BEGIN_DECLS

/**
 * OE_RENDER_ERROR: error domain for render failures.
 */
#define OE_RENDER_ERROR (oe_render_error_quark ())

GQuark oe_render_error_quark (void);

/**
 * OeRenderError:
 * @OE_RENDER_ERROR_OPEN_FAILED: a source file could not be opened or
 *     carries no decodable video stream, or a media reference has no
 *     file path.
 * @OE_RENDER_ERROR_DECODE_FAILED: a frame could not be decoded or
 *     scaled.
 */
typedef enum
{
  OE_RENDER_ERROR_OPEN_FAILED,
  OE_RENDER_ERROR_DECODE_FAILED,
} OeRenderError;

/**
 * OeRenderResolveFunc: resolves a clip's media reference to a file
 * path. Returns a newly allocated absolute path (transfer full) or
 * NULL when the reference cannot be resolved.
 */
typedef gchar *(*OeRenderResolveFunc) (guint media_ref, gpointer user_data);

/**
 * OeRenderSource: what to render. @sequence is a borrowed deep-copied
 * snapshot — it must outlive every session and call built from it.
 */
typedef struct
{
  const OeSequence *sequence;
  OeRenderResolveFunc resolve_path;
  gpointer resolve_data;
} OeRenderSource;

/**
 * OeRenderSession: reusable decode state for one render source. One
 * decoder per source path, kept open across frame_at calls.
 */
typedef struct _OeRenderSession OeRenderSession;

/**
 * oe_render_frame_at:
 * @source: render source (sequence snapshot + resolver)
 * @t_us: sequence time in microseconds
 * @out_w: canvas width (> 0, even keeps everything encoder-friendly)
 * @out_h: canvas height (> 0)
 * @error: return location for a #OeRenderError
 *
 * One-shot render: creates a session, renders the frame, frees the
 * session. For a run of frames prefer the session API — this is the
 * same code path, just without decoder reuse.
 *
 * Returns: (transfer full): an owned B8G8R8A8 canvas, @out_w * @out_h
 * pixels, stride @out_w * 4, fully opaque; free with g_free(). NULL
 * on failure with @error set.
 */
guint8 *oe_render_frame_at (const OeRenderSource *source, gint64 t_us, int out_w, int out_h,
                            GError **error);

/**
 * oe_render_blend_channel: the compositor's pure channel blend —
 * straight (non-premultiplied) integer src-over. @src_c over @dst_c
 * weighted by @src_a (0-255 source alpha, already opacity-scaled);
 * result rounded via +127 bias. Deterministic across builds and
 * platforms: integer arithmetic only, no FP.
 */
guint8 oe_render_blend_channel (guint8 dst_c, guint8 src_c, guint8 src_a);

/**
 * oe_render_session_new:
 * @source: render source (sequence snapshot + resolver)
 *
 * Returns: (transfer full): a new session; free with
 * oe_render_session_free().
 */
OeRenderSession *oe_render_session_new (const OeRenderSource *source);

/**
 * oe_render_session_frame_at:
 * @session: a session built from the same source
 * @t_us: sequence time in microseconds
 * @out_w: canvas width (> 0)
 * @out_h: canvas height (> 0)
 * @error: return location for a #OeRenderError
 *
 * Times at or after the previously rendered source position decode
 * forward without seeking; earlier times use the decode-at discipline.
 * A source whose stream ends holds its last decoded frame (still
 * images render at any sequence time this way).
 *
 * Returns: (transfer full): an owned B8G8R8A8 canvas as in
 * oe_render_frame_at(); NULL on failure with @error set.
 */
guint8 *oe_render_session_frame_at (OeRenderSession *session, gint64 t_us, int out_w, int out_h,
                                    GError **error);

/**
 * oe_render_session_free: closes every cached decoder.
 */
void oe_render_session_free (OeRenderSession *session);

/**
 * oe_render_chroma_key_alpha: (Phase 11 Wave A) the per-pixel rule
 * behind source-space chroma keying — the alpha @key assigns to one
 * pixel with RGB (@r, @g, @b). Tolerance and softness ride the
 * 0-1024 fade/gain domain; the distance metric is Euclidean RGB in
 * 255ths fixed point with exactly one rounding on the soft ramp.
 * Pure and integer-exact; exported for the titles-key suite.
 */
guint8 oe_render_chroma_key_alpha (const OeClipKey *key, gint r, gint g, gint b);

G_END_DECLS
