# Phase 9 Wave A — Visual properties and layered compositing

*The guided walkthrough for the compositing phase's first wave: per-clip
visual properties, the layered compositor behind one shared render seam,
and the inspector that edits them — with Wave B (keyframes, transitions,
fade envelopes) deferred but reserved for.*

## 1. Phase purpose

Through Phase 8, a video track renders its one covering clip and the
highest-index track simply wins. That is a slideshow rule, not a
compositor: no way to shrink a clip into a corner, fade it, or put two
tracks on screen at once. Phase 9 Wave A adds the per-clip transform
model and the layered compositor that consumes it, under one
compatibility constraint that shapes every file in the wave:

> **A clip with the identity visual renders byte-identically to how it
> rendered before the phase.** Zero-value means today's behavior — in
> the model, in the JSON, and in the compositor's fast path — so
> existing projects, existing tests, and the existing preview/export
> parity cannot drift.

The wave delivers five things: the `OeClipVisual` sub-struct on every
clip with identity defaults, deep copies, and a validated mutator; the
layered compositor in `oe_render.c` (ascending track order, crop →
scale → rotate → translate, straight integer src-over) with the
single-default-transform fast path kept untouched; the monitor's move
onto that same seam through an owned render session (one decoder cache
shared with export); the inspector's third stack page with
preview-then-commit visual editing that lands exactly one undo record
per stroke; and the clip-level `visual` JSON member following the
width/height backfill recipe — always written, identity on absence,
closed member list, no version bump.

The self-check contract held: zero-warning `-Werror` build, 16/16
plain suites, 16/16 under ASan/UBSan, 16/16 under Valgrind with the
existing suppression file (no new suppressions), full-tree
clang-format, and `scripts/run-headless.sh` exit 0.

## 2. Per-file explanations

- `src/core/oe_project.[ch]` — the model grows an owned `OeClipVisual`
  inside every `OeClip`: `pos_x`/`pos_y` (frame-pixel offsets from the
  centered anchor), `scale_permille` (1000 = 1.0×), `rotation_cdeg`
  (1/100 degree), `opacity` (0–255), `crop_l/t/r/b` (source pixels),
  `fade_in_us`/`fade_out_us` (dormant until Wave B), and a keyframe
  store pointer (present, unused in Wave A — validation rejects a
  non-NULL store so Wave B cannot silently half-work). Identity is
  `pos = (0,0)`, scale 1000, rotation 0, opacity 255, no crop. The
  copy trio (`clip_new` / `clip_free` / `oe_track_copy`) deep-copies
  the visual; `oe_clip_visual_equal` is a full `memcmp` (keyframes
  must be NULL on both sides) and `oe_clip_visual_is_default` compares
  against identity. The mutator `oe_project_set_clip_visual` validates
  first (`OE_PROJECT_ERROR_BAD_VISUAL` for out-of-domain scale,
  rotation, or a non-NULL keyframe store; `BAD_CLIP` for bad indices),
  then mutates and notifies exactly once.
- `src/media/oe_render.c` — the seam becomes the compositor.
  `collect_covering` gathers every covering video clip across tracks;
  one clip with the identity visual takes the untouched Phase 8
  pipeline (box-fit + centered copy) byte-identically; several take
  the layered loop: decode → crop (source pixels, before scaling) →
  nearest-neighbor scale → integer bilinear rotation (8-bit weights,
  deterministic) → translate to the centered anchor plus the position
  offset → straight non-premultiplied integer src-over
  (`oe_render_blend_channel`). Opacity-0 layers skip out. No FFmpeg
  filter strings anywhere — the pipeline is integer C.
- `src/app/oe_playback_session.c` — the monitor stops owning a
  single path-keyed decoder and instead owns an `OeRenderSession` over
  its deep-copied sequence snapshot, lazily created and invalidated
  when the snapshot refreshes. Same-frame deduplication is preserved;
  a project-observer notification (property edits) triggers a one-shot
  `oe_render_frame_at` repaint of the paused monitor. Export keeps
  rendering at sequence size; the monitor keeps the 720p decode box.
- `src/app/oe_undo_stack.c` — new record kind `OE_UNDO_OP_VISUAL`
  appended at the end of `OeUndoOpKind` (the enum order is part of the
  persistence and test contract). The stroke record carries the
  baseline clip plus the final visual; undo applies the baseline,
  redo the final. `oe_edit_set_clip_visual` (one-shot: capture,
  mutate, record) and `oe_edit_set_clip_visual_with_old` (explicit
  baseline for previewed strokes) route through the validated
  mutator; a stroke that ends where it started records nothing.
- `src/core/oe_project_format.c` — the clip object gains `visual`:
  the writer emits all eleven integer members on every clip; the
  reader backfills identity when the member is absent, and applies
  the closed-schema rules inside it (`MISSING`/`UNKNOWN_MEMBER`/
  `TYPE`/`VALUE` per member). Integer tokens only —
  `"scale-permille": 1.25` is a `TYPE` error; `1250` is the spelling.
- `src/ui/oe_main_window.c` — the inspector's third stack page
  (`clip`) shows when the timeline selection is non-empty, with
  sliders/spin buttons for the visual properties. Drags and spins
  preview through unrecorded `oe_project_set_clip_visual` calls;
  release (or activate) commits exactly one
  `oe_edit_set_clip_visual_with_old` record with the stroke baseline.
- `tests/test_project.c`, `tests/test_undo_stack.c`,
  `tests/test_project_format.c`, `tests/test_export.c` — the four
  existing suites absorb the new coverage (identity defaults, mutator
  validation, deep-copy equivalence; stroke records and zero-delta
  suppression; visual round trips and backfill; blend unit tests, the
  two-layer seam, and export decode-back parity). The suite count
  stays sixteen because no new binary was needed.

## 3. Block-by-block build walkthrough

The wave lands as a sequence of verticals, each green on its own:

1. **Model.** `OeClipVisual` fields, identity/validation/equality
   helpers, the copy trio, and the validated mutator. The battery
   stays green because every inserted clip starts at identity — the
   compositor does not read the fields yet.
2. **Compositor.** `collect_covering`, the transform pipeline, the
   blend, and the fast path. The straight-cut parity test passes
   untouched: single covering clips with identity visuals never leave
   the old path.
3. **Monitor.** The playback session adopts the render session and
   the paused-repaint notification.
4. **UI/undo.** The enum append, both recorder entry points, and the
   inspector page with its preview-then-commit contract.
5. **Persistence.** The `visual` member: canonical writer, backfill
   reader, closed member list, byte-identical save-load-save.
6. **Tests.** The equivalence harness: blend exactness to ±1, the
   two-layer seam, and two-layer export decode-back parity at the
   documented |Δ| ≤ 8 block-mean tolerance.
7. **Docs.** This file, the architecture section, the code map, the
   glossary, and the format guide.

## 4. C concepts in play

- **Owned sub-structs.** `OeClipVisual` lives inside `OeClip` by
  value, but its keyframe store pointer is owned. Every value copy
  site must be audited: a shallow struct copy of a clip would alias
  the store. The copy trio centralizes this.
- **Integer-only graphics math.** Scale in permille and rotation in
  centidegrees keep the model, the mutator, and the JSON integral.
  The rotation resample uses 8-bit bilinear weights so results are
  deterministic across builds and platforms — no floating-point
  rounding modes in the render path.
- **Straight vs premultiplied alpha.** The compositor blends straight
  (`out = (src·a + dst·(255−a))/255`) because source pixels are never
  premultiplied; premultiplying at decode would lose precision for
  opaque video (a = 255) and complicate the pure-channel unit test.
- **Snapshots and invalidation.** The render source borrows a deep
  sequence copy. Both the monitor and the exporter treat the snapshot
  as immutable; edits invalidate the monitor's session so the next
  frame re-snapshots. Tests that mutate-then-render must re-snapshot
  too — see the bug log.
- **Enum appends are contracts.** `OE_UNDO_OP_VISUAL` is appended
  last so persisted-kind ordering and every switch's fallthrough
  behavior stay stable.

## 5. Ownership table

| Object | Allocated by | Owned by | Freed by |
|---|---|---|---|
| `OeClipVisual` (value part) | clip construction | the `OeClip` | `clip_free` with the clip |
| Keyframe store pointer | Wave B (never in A) | the `OeClipVisual` | `clip_free` (must stay NULL in A) |
| Scaled/rotated layer buffers | `compose_layered` per clip per frame | the frame pipeline | `g_free` at pipeline end |
| Composited canvas | `compose_layered` / fast path | the caller | caller's `g_free` |
| Monitor render session | first frame render | the playback session | session invalidation / teardown |
| Sequence snapshot for render | session refresh | the playback session | next refresh or teardown |
| Visual undo record payload | recorder helpers | the undo stack | depth eviction / truncation |

The bug log's first entry is the one row of this table that was first
written wrong.

## 6. Call flow

```
[inspector drag]                    [paused monitor repaint]
  spin/slider changed                 project observer (one-shot)
  │                                   │
  ├─ preview: oe_project_set_clip_visual      (no record)
  │     └─ notify observers ──────────────►  one-shot oe_render_frame_at
  │                                          └─ oe_render_session_frame_at
  │                                             ├─ collect_covering
  │                                             ├─ fast path (identity)
  │                                             └─ or compose_layered:
  │                                                decode → crop → scale
  │                                                → rotate → translate
  │                                                → blend (src-over)
  └─ release: oe_edit_set_clip_visual_with_old
        └─ one OE_UNDO_OP_VISUAL record
              ├─ undo → stroke baseline
              └─ redo → final visual
```

Export samples the same seam frame-by-frame over its worker thread;
nothing in `oe_export.c` changed in this wave.

## 7. Alternatives considered

- **Per-clip effects as FFmpeg filter graphs.** Rejected: filter
  strings are opaque, hard to unit test, and nondeterministic across
  FFmpeg builds. The locked decision D2 keeps the render path pure
  integer C with one pure channel-blend function that the equivalence
  suite can pin to ±1.
- **Premultiplied-alpha canvas.** Rejected for Wave A: source video
  is opaque, so premultiplication buys nothing yet and costs precision
  plus a second convention. The blend signature keeps the door open.
- **Floating-point transforms.** Rejected: deterministic behavior
  across platforms is a spec requirement; integer bilinear weights
  (8-bit) are testable to exactness and are what the parity tolerance
  is written against.
- **A separate preview compositor for the monitor.** Rejected
  outright by the Phase 8 rule — one seam or none. The monitor now
  literally uses `OeRenderSession`, the same object the exporter
  drives.
- **Keyframes editable in Wave A.** Deferred to Wave B per the spec:
  the store field exists and validation rejects non-NULL, so nothing
  can half-exist.

## 8. Bug log

Bugs found and fixed while building the wave:

- **Uninitialized staging copy in the mutator.** The first
  `oe_project_set_clip_visual` staged the new visual through a
  non-identity-initialized temporary and then copied it into the clip,
  so a partial validation failure could leave garbage in clip state.
  Fix: identity-initialize the staging visual before copying.
- **Stroke baseline captured from the wrong state.** The first
  recorder stored "the clip as it is at record time" as the undo
  payload — but a previewed stroke has already left the model at its
  LAST preview state, so undo restored the preview, not the stroke
  start. The test suite caught it: fix is to store the caller's
  explicit baseline (the visual at the stroke's first change) in the
  record, keeping the last-preview state out of the payload.
- **Test-side stale snapshot.** The two-layer seam test snapshotted
  the sequence once and then mutated visuals — the render source kept
  rendering the pre-edit snapshot (identity), so scale edits appeared
  to be ignored. The code was right; the test must re-snapshot after
  each edit, exactly like the monitor's session invalidation. The
  failure is preserved as a comment in the test because the same
  mistake is the natural first bug for every future consumer of the
  seam.

## 9. What is next

Wave B inherits the reserved fields and the seam extension point:

- **Keyframes.** The store pointer, validation gate, and JSON
  backfill are in place; Wave B adds timed property interpolation and
  the reading side of the render path.
- **Transitions.** Cross-dissolves and dip-to-black are boundary
  objects between clips; the layered loop already renders partial
  coverage and opacity, so a transition becomes a deterministic
  alpha schedule over the same blend.
- **Fades.** `fade_in_us`/`fade_out_us` are persisted today and
  ignored by the renderer; Wave B consumes them as per-frame opacity
  envelopes.
- **Rotation quality.** The 8-bit-weight bilinear resample is
  deterministic; a higher-quality path would be an additive alternative
  behind the same seam.

## 10. Five review questions (with answers)

1. **Why can a zero-value `OeClipVisual` not break old behavior?**
   Because identity is the zero value except scale (1000), inserted
   clips get identity by construction, the compositor's fast path
   treats identity-visual single clips as exactly the Phase 8
   pipeline, and the JSON reader backfills identity for absent
   members. The straight-cut parity test — untouched from Phase 8 —
   is the pin.
2. **Why does undo restore the stroke baseline instead of the last
   preview?** A previewed stroke leaves the model at its last preview
   state; capturing "current state" at record time therefore captures
   the preview, and undo would appear to do nothing. The record
   carries the visual at the stroke's first change — the only state
   undo can mean.
3. **Where does track order become visible to the user?** In the
   compositor's blend order (ascending, bottom first) and therefore
   in the timeline's track stack: the top track's clips paint over
   lower ones wherever both cover. The two-layer seam test proves
   both directions: topmost wins inside its footprint, fallthrough
   outside it.
4. **Why is the export tolerance |Δ| ≤ 8 per channel and not exact?**
   The exported file passes through yuv420p chroma subsampling and
   x264 quantization; per-pixel equality is impossible by
   construction. The test asserts exactness where it exists (blend
   unit ±1, dominant-color class) and 8×8 block means within |Δ| ≤ 8
   where the codec dominates, with the reason documented in-test.
5. **What stops Wave B from half-shipping?** The keyframe store is
   validated to be NULL in Wave A, the fade fields are persisted and
   deliberately unused, and the enum append keeps `OE_UNDO_OP_VISUAL`
   stable. Wave B's work is additive on top of a seam that cannot
   silently change Wave A's rendered output.
