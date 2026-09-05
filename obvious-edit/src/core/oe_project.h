/* oe_project.h — the project & timeline data model (Phase 3).
 *
 * OeProject owns the editing session's data: a sequence (frame rate +
 * ordered tracks of clips that reference media non-destructively) and
 * the file-stable media references those clips point at. It follows the
 * OeMediaLibrary idiom exactly:
 *
 *   - single plain-function-pointer observer, fired exactly once per
 *     successful mutation, on the calling (main) thread;
 *   - main-thread-only mutation — there is no locking, by contract;
 *   - deep-copy getters — callers own their copies, the model shares
 *     nothing;
 *   - destruction order documented in the implementation, with no
 *     callbacks during teardown.
 *
 * Model invariants:
 *   - clips within a track are ordered by position_us;
 *   - a clip's timeline duration is source_out_us - source_in_us for
 *     EVERY media kind, stills included — a still's source range
 *     encodes its screen duration instead of a probe duration
 *     (default 5 s at insert; see
 *     OE_PROJECT_STILL_DEFAULT_DURATION_US);
 *   - overlapping inserts on one track are rejected with a typed
 *     error; adjacent clips (end == start) are fine;
 *   - gaps are the absence of clips — there are no placeholder
 *     elements;
 *   - the tracks array's order IS the layering: for video tracks,
 *     higher index composites above; for audio tracks, higher index
 *     mixes above (later contributors win ties).
 *
 * GTK-free: GLib/GObject only, headless-testable. Nothing here ever
 * includes gtk.h, FFmpeg, or SDL headers.
 */

#pragma once

#include <glib-object.h>
#include <glib.h>

#include "oe_time.h"

G_BEGIN_DECLS

#define OE_TYPE_PROJECT (oe_project_get_type ())
G_DECLARE_FINAL_TYPE (OeProject, oe_project, OE, PROJECT, GObject)

/** Default sequence rate for brand-new projects (matches the schema example). */
#define OE_PROJECT_DEFAULT_RATE_NUM 25
#define OE_PROJECT_DEFAULT_RATE_DEN 1

/** Recommended source range length when inserting a still image (5 s of screen time). */
#define OE_PROJECT_STILL_DEFAULT_DURATION_US G_GINT64_CONSTANT (5000000)

/** Default output resolution for new sequences (even: yuv420p export compatibility). */
#define OE_SEQUENCE_DEFAULT_WIDTH 1920
#define OE_SEQUENCE_DEFAULT_HEIGHT 1080

/**
 * OeClipVisual: per-clip picture geometry and opacity (Phase 9 Wave A).
 *
 * Integers only, locked decision D1: @pos_x/@pos_y are frame-pixel
 * offsets from the frame center (centered anchor), @scale_permille is a
 * uniform scale in permille (1000 = 1.0x), @rotation_cdeg is rotation
 * in 1/100 degree (clockwise), @opacity is 0-255 straight alpha,
 * @crop_l/@crop_t/@crop_r/@crop_b are source-pixel crop edges trimmed
 * before scaling, and @fade_in_us/@fade_out_us are audio fade lengths
 * (dormant: consumed by Wave B envelopes, never by Wave A rendering).
 * @keyframes is the Wave B per-property keyframe store — NULL in Wave
 * A, an invariant every copy path enforces.
 *
 * The identity state (oe_clip_visual_identity) reproduces the
 * pre-Phase-9 render exactly.
 */
typedef struct
{
  gint pos_x;
  gint pos_y;
  guint scale_permille;
  gint rotation_cdeg;
  guint8 opacity;
  guint crop_l;
  guint crop_t;
  guint crop_r;
  guint crop_b;
  guint64 fade_in_us;
  guint64 fade_out_us;
  GArray *keyframes; /* NULL in Wave A: the store arrives in Wave B */
} OeClipVisual;

/**
 * OeClip: one non-destructive source-range placement on a track.
 * @position_us: sequence position (>= 0) of the clip's start.
 * @source_in_us: first source microsecond played (>= 0).
 * @source_out_us: one past the last source microsecond; always >
 *     @source_in_us. The clip's timeline duration is
 *     @source_out_us - @source_in_us for every media kind, stills
 *     included (a still's source range encodes screen duration).
 * @media_ref: file-stable media reference owned by the project — never
 *     a session asset id (those are transient and never serialize).
 * @visual: owned picture geometry/opacity; deep-copied with the clip,
 *     never aliased (Wave B keyframes included via the NULL-store
 *     invariant enforced in Wave A).
 */
typedef struct
{
  gint64 position_us;
  gint64 source_in_us;
  gint64 source_out_us;
  guint media_ref;
  OeClipVisual visual;
} OeClip;

/**
 * oe_clip_visual_identity: the default state — zeroed position/crop,
 * 1000 permille scale, 0 rotation, full opacity, no fades, no
 * keyframes. Every OeClip starts here.
 */
OeClipVisual oe_clip_visual_identity (void);

/**
 * oe_clip_visual_clear: frees owned members and zeroes @visual (safe
 * on a zeroed struct, like the model's other clear helpers).
 */
void oe_clip_visual_clear (OeClipVisual *visual);

/**
 * oe_clip_visual_copy: deep-copies @src into @dst, clearing @dst's
 * owned members first. Wave A enforces the NULL keyframe-store
 * invariant on both sides.
 */
void oe_clip_visual_copy (OeClipVisual *dst, const OeClipVisual *src);

/**
 * oe_clip_visual_equal: field equality; owned stores compared by the
 * Wave A invariant (both NULL).
 */
gboolean oe_clip_visual_equal (const OeClipVisual *a, const OeClipVisual *b);

/**
 * oe_clip_visual_is_default: TRUE when @visual is exactly the identity
 * state (the compositor's fast-path trigger).
 */
gboolean oe_clip_visual_is_default (const OeClipVisual *visual);

/**
 * oe_clip_visual_is_valid: domain check for the validated mutator and
 * the persistence reader — scale in [1, 32000] permille, rotation in
 * [-36000, 36000] centidegrees, opacity 0-255, non-negative crops and
 * fades, NULL keyframe store.
 */
gboolean oe_clip_visual_is_valid (const OeClipVisual *visual);

/**
 * OeTrackKind: the two parallel lane kinds of a sequence.
 * @OE_TRACK_VIDEO: compositing order = array order (higher index above).
 * @OE_TRACK_AUDIO: mixing order = array order (higher index wins ties).
 */
typedef enum
{
  OE_TRACK_VIDEO,
  OE_TRACK_AUDIO,
} OeTrackKind;

const gchar *oe_track_kind_get_name (OeTrackKind kind);

/**
 * OeTrack: one lane of the sequence. @clips is a GPtrArray of owned
 * #OeClip pointers, ordered by position.
 */
typedef struct
{
  OeTrackKind kind;
  GPtrArray *clips;
} OeTrack;

/**
 * OeSequence: the whole timeline: one frame rate, an output size, and
 * ordered tracks. @tracks is a GPtrArray of owned #OeTrack pointers.
 */
typedef struct
{
  OeRational frame_rate;
  gint width;
  gint height;
  GPtrArray *tracks;
} OeSequence;

/* Value-type trio for the model structs, mirroring OeAssetInfo: init
 * zeroes, clear frees owned data (safe on a zeroed struct), copy
 * deep-copies (clearing @dst first). */
void oe_track_init (OeTrack *track);
void oe_track_clear (OeTrack *track);
void oe_track_copy (OeTrack *dst, const OeTrack *src);
void oe_sequence_init (OeSequence *sequence);
void oe_sequence_clear (OeSequence *sequence);
void oe_sequence_copy (OeSequence *dst, const OeSequence *src);

/**
 * OE_PROJECT_ERROR: error domain for model mutation failures.
 */
#define OE_PROJECT_ERROR (oe_project_error_quark ())

GQuark oe_project_error_quark (void);

/**
 * OeProjectError:
 * @OE_PROJECT_ERROR_OVERLAP: the placement would overlap an existing
 *     clip on the same track; the error names the clip it hit.
 * @OE_PROJECT_ERROR_BAD_RANGE: negative position, empty or negative
 *     source range, or a placement past the representable timeline.
 * @OE_PROJECT_ERROR_BAD_TRACK: track index out of range.
 * @OE_PROJECT_ERROR_BAD_CLIP: clip index out of range.
 * @OE_PROJECT_ERROR_UNKNOWN_MEDIA: the clip names a media reference
 *     the project does not hold.
 * @OE_PROJECT_ERROR_DUPLICATE_REF: a media reference number is
 *     already taken.
 * @OE_PROJECT_ERROR_BAD_SIZE: a sequence width or height is out of
 *     domain (must be positive and even).
 * @OE_PROJECT_ERROR_BAD_VISUAL: a clip visual is out of domain —
 *     scale, rotation, opacity, crop, or fade beyond the documented
 *     ranges (see oe_clip_visual_is_valid).
 */
typedef enum
{
  OE_PROJECT_ERROR_OVERLAP,
  OE_PROJECT_ERROR_BAD_RANGE,
  OE_PROJECT_ERROR_BAD_TRACK,
  OE_PROJECT_ERROR_BAD_CLIP,
  OE_PROJECT_ERROR_UNKNOWN_MEDIA,
  OE_PROJECT_ERROR_DUPLICATE_REF,
  OE_PROJECT_ERROR_BAD_SIZE,
  OE_PROJECT_ERROR_BAD_VISUAL,
} OeProjectError;

/**
 * OeProjectChangedFunc: observer for any project mutation. Fires
 * exactly once per successful mutation, on the calling thread.
 * @user_data: context pointer supplied at connect time.
 */
typedef void (*OeProjectChangedFunc) (gpointer user_data);

/**
 * oe_project_new:
 * @frame_rate: sequence frame rate; must satisfy the #OeRational
 *     invariant (den > 0, reduced) — build it with oe_time_rate().
 *
 * Returns: (transfer full): a new empty project named "Untitled" with
 * no tracks and no media.
 */
OeProject *oe_project_new (OeRational frame_rate);

/**
 * oe_project_new_default:
 *
 * Returns: (transfer full): a new project at
 * #OE_PROJECT_DEFAULT_RATE_NUM / #OE_PROJECT_DEFAULT_RATE_DEN.
 */
OeProject *oe_project_new_default (void);

/**
 * oe_project_set_observer:
 * @observer: change sink, or NULL to clear
 *
 * Single observer slot (the window's UI projection). The observer is
 * never invoked from dispose.
 */
void oe_project_set_observer (OeProject *project, OeProjectChangedFunc observer,
                              gpointer user_data);

/**
 * oe_project_get_name:
 *
 * Returns: the project name, borrowed — valid until the next
 *     oe_project_set_name call or the project is freed.
 */
const gchar *oe_project_get_name (OeProject *project);
void oe_project_set_name (OeProject *project, const gchar *name);

/**
 * oe_project_get_sequence:
 * @out: receives a freshly initialized deep copy; the caller owns it
 *     and must clear it with oe_sequence_clear().
 *
 * Deep-copy getter: mutating the project afterwards never changes the
 * copy, and freeing the copy never disturbs the project.
 */
void oe_project_get_sequence (OeProject *project, OeSequence *out);

/**
 * oe_project_set_sequence_size:
 * @width: output frame width in pixels (> 0, even — yuv420p export
 *     compatibility).
 * @height: output frame height in pixels (> 0, even).
 *
 * Fires the observer exactly once on success; a rejected size leaves
 * the model untouched and the observer silent.
 *
 * Returns: TRUE when the size was applied.
 */
gboolean oe_project_set_sequence_size (OeProject *project, gint width, gint height, GError **error);

/**
 * oe_project_get_clip_count:
 * @track_index: track to count
 *
 * Read-only view over the live track (no copy): the clip count, in
 * position order. 0 when @track_index is out of range.
 *
 * Returns: the number of clips on the track.
 */
guint oe_project_get_clip_count (OeProject *project, guint track_index);

/**
 * oe_project_get_clip:
 * @out: receives a value copy of the clip (OeClip owns no memory, so
 *     the struct copy IS the deep copy)
 *
 * Single-clip deep-copy getter: reads one clip's exact fields without
 * copying the whole sequence — the undo recorder captures pre-edit
 * state with it (oe_undo_stack.h).
 *
 * Returns: TRUE when both indices are in range.
 */
gboolean oe_project_get_clip (OeProject *project, guint track_index, guint clip_index, OeClip *out);

/**
 * oe_project_set_clip_visual: the validated mutator for a clip's
 * visual state. Rejects out-of-domain visuals with
 * #OE_PROJECT_ERROR_BAD_VISUAL (and out-of-range indices with the
 * usual typed errors) without touching the model; on success swaps in
 * a deep copy and notifies the observer exactly once.
 *
 * Returns FALSE (with @error set) when the mutation was rejected.
 */
gboolean oe_project_set_clip_visual (OeProject *project, guint track_index, guint clip_index,
                                     const OeClipVisual *visual, GError **error);

/**
 * oe_project_add_track:
 *
 * Appends an empty track of @kind (index = count before the call) and
 * notifies the observer.
 *
 * Returns: the new track's index.
 */
guint oe_project_add_track (OeProject *project, OeTrackKind kind);

guint oe_project_get_track_count (OeProject *project);

/**
 * oe_project_insert_clip:
 * @track_index: destination track.
 * @media_ref: a media reference added with oe_project_add_media().
 * @position_us: sequence position (>= 0) of the clip start.
 * @source_in_us / @source_out_us: source range, @source_out_us >
 *     @source_in_us >= 0. For still images pass the screen duration as
 *     the range (default #OE_PROJECT_STILL_DEFAULT_DURATION_US).
 *
 * Inserts in position order and notifies the observer. Overlaps on the
 * destination track are rejected with OE_PROJECT_ERROR_OVERLAP;
 * adjacency (clip end == next start) is allowed.
 *
 * Returns: TRUE on success, FALSE with @error set on rejection.
 */
gboolean oe_project_insert_clip (OeProject *project, guint track_index, guint media_ref,
                                 gint64 position_us, gint64 source_in_us, gint64 source_out_us,
                                 GError **error);

/**
 * oe_project_move_clip:
 *
 * Moves the clip at @clip_index on @track_index to @position_us
 * (keeping its source range and length), preserving position order.
 * The clip's current footprint does not count as an overlap target;
 * other clips do. Fires the observer exactly once on success.
 *
 * Returns: TRUE on success, FALSE with @error set on rejection.
 */
gboolean oe_project_move_clip (OeProject *project, guint track_index, guint clip_index,
                               gint64 position_us, GError **error);

/**
 * oe_project_remove_clip:
 *
 * Drops the clip (gaps are absence, nothing fills in) and notifies
 * the observer.
 *
 * Returns: TRUE on success, FALSE with @error set when the indices are
 * out of range.
 */
gboolean oe_project_remove_clip (OeProject *project, guint track_index, guint clip_index,
                                 GError **error);

/**
 * oe_project_trim_clip:
 * @track_index: track holding the clip
 * @clip_index: clip within the track
 * @new_source_in: replacement source-in, µs from source start
 * @new_source_out: replacement source-out, exclusive
 * @error: failure domain #OE_PROJECT_ERROR
 *
 * Re-trims the clip's source range in place. Sequence position is
 * untouched: trimming never shifts neighbouring clips. Requires
 * 0 <= @new_source_in < @new_source_out (else
 * #OE_PROJECT_ERROR_BAD_RANGE). For AV media the range must lie inside
 * the probed source duration annotated with
 * oe_project_set_media_source_duration() (else
 * #OE_PROJECT_ERROR_BAD_RANGE); stills may extend freely — a still's
 * source range encodes screen duration (the uniform-duration rule).
 *
 * The timeline duration is the source range for every media kind, so a
 * trim changes the clip's footprint like any duration edit: a trim
 * that would grow across a neighbour is rejected with
 * #OE_PROJECT_ERROR_OVERLAP, exactly as a move or insert is. The
 * widget's drag preview clamps to source bounds, so interactive trims
 * stay clear of this check; the model remains the authority.
 *
 * Fires the observer exactly once on success, on the calling thread.
 *
 * Returns: TRUE on success, FALSE with @error set on rejection.
 */
gboolean oe_project_trim_clip (OeProject *project, guint track_index, guint clip_index,
                               gint64 new_source_in, gint64 new_source_out, GError **error);

/**
 * oe_project_add_media:
 * @path: file path to reference (copied).
 *
 * Assigns the next file-stable reference number and notifies the
 * observer.
 *
 * Returns: the new reference number (starts at 1).
 */
guint oe_project_add_media (OeProject *project, const gchar *path);

/**
 * oe_project_add_media_ref:
 * @ref: explicit reference number (> 0), as read from a document.
 *
 * Loader-side variant of oe_project_add_media() that preserves the
 * document's reference numbers. Moves the next-reference counter past
 * @ref so later automatic assignments never collide.
 *
 * Returns: TRUE on success; FALSE with OE_PROJECT_ERROR_DUPLICATE_REF
 * when @ref is already taken.
 */
gboolean oe_project_add_media_ref (OeProject *project, guint ref, const gchar *path,
                                   GError **error);

guint oe_project_get_media_count (OeProject *project);

/**
 * oe_project_get_media:
 * @index: 0 .. oe_project_get_media_count() - 1.
 * @ref: receives the reference number, or NULL to ignore.
 * @path: receives a copy of the path (caller frees), or NULL to
 *     ignore.
 *
 * Returns: TRUE when @index is in range.
 */
gboolean oe_project_get_media (OeProject *project, guint index, guint *ref, gchar **path);

/**
 * oe_project_dup_media_path:
 * @ref: media reference number.
 *
 * Returns: (transfer full): a copy of the referenced path, or NULL
 * when @ref is unknown.
 */
gchar *oe_project_dup_media_path (OeProject *project, guint ref);

/**
 * oe_project_set_media_source_duration:
 * @ref: a media reference in the project (unknown refs are ignored)
 * @source_duration_us: probed duration of the referenced file in µs;
 *     0 marks a still — or any source whose range may extend freely
 *
 * Session-state annotation backing trim validation. Probe metadata is
 * regenerable and never serializes (project-format.md), so the app
 * layer calls this as probe results arrive, again after every project
 * Open re-import. A positive duration bounds AV trims; 0 means
 * unbounded, because a still's source range encodes screen duration
 * instead. Media never annotated (no probe verdict yet) is treated as
 * unbounded too: the model never invents a bound it was not told.
 *
 * Annotation is session state, not a document mutation: it never fires
 * the observer and never serializes.
 */
void oe_project_set_media_source_duration (OeProject *project, guint ref,
                                           gint64 source_duration_us);

/**
 * oe_project_get_media_source_duration:
 * @source_duration_us: receives the annotated duration (0 for stills
 *     and unannotated media), or NULL to ignore
 *
 * Returns: TRUE when @ref is known.
 */
gboolean oe_project_get_media_source_duration (OeProject *project, guint ref,
                                               gint64 *source_duration_us);

G_END_DECLS
