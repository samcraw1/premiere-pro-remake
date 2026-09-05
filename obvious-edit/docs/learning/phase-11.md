# Phase 11 Wave A — Titles, solids, and chroma key: the hermetic core

*The guided walkthrough for the generator phase's first wave: the
closed clip-kind model with validated generator/key mutation, the
Cairo title/solid rasterizer with its session-owned cache, the
source-space alpha-only chroma key, the kind-aware fast path, undo,
persistence, and the 19th test suite — with the inspector UI, timeline
paint, and command affordances deferred to Wave B.*

## 1. Phase purpose

Through Phase 10 every pixel on the timeline was decoded from a file.
Phase 11 Wave A adds the first generated content — titles and solid
color fills — plus the first per-clip keying, under the same
compatibility constraint that shaped every earlier wave:

> **Every pre-Phase-11 project renders byte-identically after the
> upgrade.** The identity kind is `media`, the identity generator is
> the empty payload, the identity key is disabled — in the model, in
> the JSON backfills, and in the fast-path guard — so the pinned
> parity tests stay untouched and nothing changes for projects that
> never asked for generators.

The wave delivers six things: the closed `OeClipKind` with
`OeClipGenerator` (owned text) and memory-free `OeClipKey` riding
`OeClip` exactly like the visual/audio substructs; the validated
`oe_project_insert_generator_clip` / `set_clip_generator` /
`set_clip_key` mutators with `OE_PROJECT_ERROR_BAD_GENERATOR` at the
error tail; the Cairo rasterizer in `src/media/oe_generator_raster.c`
rasterizing once per (identity, text, size, color) at sequence
resolution into straight-alpha BGRA with the cache owned by the
render session; the source-space, post-crop/pre-scale, ALPHA-ONLY
chroma-key rewrite inside the layered compositor; the extended
fast-path guard (kind == media AND key disabled); and the two new
undo ops plus the always-written `kind`/`generator`/`key` JSON
members under the established backfill recipe.

## 2. Decisions worth reading twice

- **Flat fields, no tagged unions.** The generator payload and key
  state ride `OeClip` as flat members exactly like `visual` and
  `audio` — every clip carries all three substructs even though a
  media clip's generator and a generated clip's key sit dormant at
  their identities. The copy trios and validation rules make the
  dormant state free; a tagged union would have forced every
  consumer to dispatch on kind forever.
- **Rasterize once, cache by identity.** A title's pixels depend on
  (clip identity, text, size, color) and the sequence resolution —
  never on the frame being composited or the monitor's canvas size.
  The session-owned cache turns a 30-minute title clip into one
  rasterization, and dropping the cache on every sequence-snapshot
  refresh is what makes the paused-repaint force-render show edited
  text instead of a stale glyph run.
- **Scaled, never box-fitted.** The monitor consumes the cached
  buffer through the same integer nearest-neighbor layer scale as
  any media layer. Box-fitting would make title quality depend on
  the preview canvas — the exact drift the shared-seam design
  forbids.
- **Alpha-only keying.** The chroma key rewrites alpha only, in
  source space, after crop and before scale; RGB channels pass
  through untouched and there is no spill suppression. The metric
  is integer RGB distance with the 0–1024 tolerance/softness
  domains and exactly one house-ratio rounding in the softness
  band — which is why the key composes with layer opacity for free
  through the existing blend and why preview and export agree.
- **The fast path earns two conjuncts.** The single-clip guard
  becomes kind == media AND key disabled. Both are true for every
  pre-Phase-11 clip, so the byte-identical regression surface the
  straight-cut parity test pins stays exactly where it was; a keyed
  clip or a generator routes to the layered compositor.
- **Keys are media-only.** `set_clip_key` rejects generated kinds —
  keying a title against itself is meaningless and keying a solid
  would erase it — and the persistence layer rejects a non-identity
  key on a generated clip for the same reason it rejects a
  generator payload on a media clip: the state would be silently
  dropped on re-save.

## 3. The gate battery held

Zero-warning `-Werror` build, all 19 suites plain and under
ASan/UBSan and Valgrind (absolute `tests/valgrind.supp` path, no new
project-code suppressions), tree-wide clang-format,
`scripts/run-headless.sh` exit 0, and the Phase 9 straight-cut
parity tests untouched and green. The new `titles-key` suite pins
the model contract, undo replay bit-exactness (owned text), JSON
round-trip/backfill/strictness, raster determinism and cache
refresh, key-math domains and edges, fast-path preservation, and
export decode-back parity (block means |Δ| ≤ 8).

## 4. Wave B — reaching the user

Wave B makes the core reachable, editable, and visible without
changing its shape. The inspector's clip page grows kind-aware
sections on the Phase 10 stroke precedent — the generated-clip
section (text/size/color for titles, color only for solids) and the
chroma-key section (media clips on video tracks only), each with
baseline-at-first-change, preview-without-record, and exactly one
undo record at commit; a zero-delta stroke records nothing and a
rejected commit reloads the model's truth. `media.insert-title` /
`media.insert-solid` land generated clips at the playhead on the
first video track and record no undo entry (the media.import
precedent). The timeline paints generated clips with a distinct
teal fill, labels them from the model — the title's text, not a
media basename — and trims them like stills (free duration,
source-range-as-duration). Every edit fires the model observer, so
the paused monitor repaints through the existing snapshot-refresh
seam with the generator cache dropped: editing title text changes
the paused monitor with no transport action. The 20th GTK-free
suite (`test_inspector_strokes.c`) replays the stroke contract
through the same validated mutators, and the Xvfb dogfood captures
the before/after evidence — no-playback sessions only, per the
documented transport-playback limitation.
