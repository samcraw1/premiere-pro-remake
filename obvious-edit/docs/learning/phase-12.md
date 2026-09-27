# Phase 12 — The razor: splitting a clip

*This walkthrough covers the razor phase. It explains the atomic
model split and the keyframe partition that keeps animation
continuous across the cut. It also covers the full-value clip insert
that makes the split's undo exact, the pointer tool on the timeline
with its hover cut line, and `Ctrl+K`, which splits the selected clip
at the playhead.*

## 1. Phase purpose

Before this phase, the editor could move, trim and ripple-delete
clips, but it could not cut one. `tool.razor` (`C`) and `tool.select`
(`V`) had been in the command registry since Phase 1 with no
handlers, so pressing them only reported "not implemented yet".
Phase 12 makes both work and adds a keyboard twin,
`edit.split-at-playhead` (`Ctrl+K`, as in Premiere).

The phase delivers:
- `oe_project_split_clip`, one validated mutation that fires the
  observer once;
- `oe_keyframes_split`, the pure keyframe partition;
- `oe_project_insert_clip_value`, which places a clip with all of its
  state;
- `OE_UNDO_OP_SPLIT` and `oe_edit_split_clip`;
- `OeTimelineTool` with the crosshair cursor, the hover cut line and
  click-to-split;
- the command handlers and an Edit-menu entry.

## 2. Decisions worth reading twice

- **Its own mutator, not trim plus insert.** `oe_project_trim_clip`
  looks up the media reference, so it rejects titles and solids
  (`media_ref` 0). `oe_project_insert_clip` always builds an identity
  media clip. Building a split from those two would lose generator
  and key state and fail on every generated clip. The split mutator
  therefore needs no media lookup. It stages both halves completely
  before changing the model, inserts the right half at
  `clip_index + 1` (index order is already position order, so no
  re-sort is needed), and notifies once.
- **Generated clips keep `[0, duration)`.** The v1 reader rejects a
  generated clip whose source-in is not zero (D3). A split title's
  right half therefore gets source range `[0, right_length)` rather
  than continuing the source offset. Media clips continue the source
  exactly where the left half stopped.
- **Keyframes stay continuous.** Keyframe times are clip-relative, so
  the right half's keys are rebased by the cut. Each half gets a
  boundary key sampled at the cut. A half that would otherwise hold a
  single key gets a held endpoint key, because a single-key run
  degrades to the static field. Inside the bracket that straddles the
  cut, the halves re-interpolate through the rounded boundary value,
  so the result can differ from the original ramp by at most one
  unit.
- **Degraded runs ride left, verbatim.** A run that samples as static
  (lone key, zero span) is never re-partitioned: splitting it could
  hand one half a run that suddenly animates. The left half keeps it
  unchanged and the right half samples static. That is exactly what
  the whole clip rendered, and the loader accepts any key time
  `>= 0`, so the document still round-trips.
- **Fades obey the hard edit.** The cut is a straight edit, so the
  fade-in stays on the left half and the fade-out moves to the right
  half, each clamped to its half's length.
- **Refuse, don't degrade, transitions.** Transitions attach to
  boundary times, and `oe_transition_window` re-derives the clip pair
  on every call. A cut strictly inside a live window would shrink a
  neighbour below the stored window, and the transition would silently
  fall back to a straight cut. The split refuses with
  `OE_PROJECT_ERROR_BAD_TRANSITION` instead. The window's own edges
  are fine.
- **Undo restores the whole clip.** The record holds a deep copy of
  the whole original clip. Undo removes the right half, then the left,
  then re-inserts the original through `oe_project_insert_clip_value`,
  keeping kind, generator, key, audio, visual and keyframes. Redo
  re-runs the split at the stored cut. The test suite pins
  byte-identical saves before and after an undo/redo pair.
- **The razor is snapped and frame-aligned.** A razor click snaps
  like a drag, to the playhead, neighbour edges and transition window
  edges, excluding the clip's own edges since a cut there is
  meaningless. The pure `oe_timeline_razor_cut` then rounds the time
  to the frame grid and rejects anything not strictly inside the
  clip. The hover line runs through the same path, so the line you
  see is the cut you get.

## 3. The gate battery

All suites pass plain, under ASan/UBSan, and under Valgrind.
Tree-wide clang-format 19 is clean. New cases:
- `/project/split*` and `/project/insert-clip-value`: the model
  contract;
- `/undo/split-*`: exact inverse, generator undo, and rejections
  leaving no record;
- `/timeline-layout/razor-cut`: frame rounding and the edge rules.

The command-table pin gains `edit.split-at-playhead`.

## 4. What is next

- Linked audio/video: the model has no link concept, so the razor
  cuts one clip at a time.
- A grouped "split every track at the playhead" undo record.
- Moving the DELETE, INSERT and ripple undo replays onto
  `oe_project_insert_clip_value`, so they stop dropping clip state.
