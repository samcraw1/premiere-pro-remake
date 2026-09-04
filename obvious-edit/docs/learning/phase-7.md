# Phase 7 — snapping and ripple: the timeline learns to cooperate

A guided walkthrough of Phase 7: what was built, why it is shaped this
way, and what to look at when you read the code.

## 1. Phase purpose

Phase 6 made every edit forgiving after the fact (undo/redo). Phase 7
makes the edit itself forgiving while you drag: the timeline quietly
corrects you onto meaningful times, and deleting a clip tidies the
track behind it instead of leaving a hole.

Concretely, three things appear:

- `src/ui/oe_timeline_layout.[ch]` (extended) — the pure snap
  decision: `OeSnapContext` (enabled flag, threshold in pixels, the
  zoom, optional playhead and frame-grid targets, same-track
  neighbour edges) and `oe_timeline_snap_time`, which returns the
  snapped candidate or the candidate unchanged. Targets: same-track
  clip edges, the playhead, zero, and frame boundaries. Nearest
  target wins; ties go to the earlier time; the 8 px threshold is
  screen-space and scales through `px_per_us`. The unconditional
  neighbour-edge snapping the clamp used to fuse in is gone —
  legality and magnetism are now separate decisions.
- `src/ui/oe_timeline.c` (changed) — the widget builds the context
  (same-track edges, the playhead when visible, the frame interval
  from the sequence rate) and applies snapping for move, trim-in,
  and trim-out BEFORE the legality clamp; the clamp stays
  authoritative and `commit_drag` still trusts the preview verbatim.
  A snapping session flag with `oe_timeline_set/get_snapping` is the
  single source of truth for on/off.
- `src/app/oe_undo_stack.[ch]` (changed) — `oe_edit_ripple_remove_clip`:
  the delete-selection handler routes through it, it removes the
  primary clip and shifts the same-track suffix left by the primary's
  duration (rigidly, gaps preserved) through the existing typed
  mutators, and it pushes ONE composite record
  (`OE_UNDO_OP_RIPPLE_DELETE`) carrying the primary's owned copy plus
  each suffix clip's pre/post positions and indices (removal
  renumbers downstream indices, so both generations are recorded).
  Undo replays descending, redo ascending — every intermediate state
  stays typed-valid.
- Wiring — `edit.snap-toggle` (accel `s`) in the command registry, an
  Edit-menu check row driven by a stateful `GSimpleAction` so the
  checkbox reflects the widget's truth, and "Snapping on/off" status
  reports through the existing seam.
- `tests/test_snap_ripple.c` — the 15th suite (18 GTK-free cases):
  per-target snap cases, inclusive band boundaries, cross-class
  tie-break, zoom scaling, the disabled pass-through, the
  snap-then-clamp pipeline, ripple first/middle/last deletes with
  JSON v1 round-trip baselines, typed rejection, one-record depth
  accounting, redo-branch clearing, and virtual-clock auto-pause for
  composite undo/redo.

What Phase 7 deliberately does **not** add: trim-to-trim snapping
candidates (edge-to-edge only for now), cross-track ripple (the
shift stays on the deleted clip's track), markers or tempo targets,
ripple on insert/move/trim, and any schema change — `.oe` files stay
format-v1 untouched, since snapping is session state and the ripple
record is history, never serialized.

## 2. Per-file explanations

| File | Role |
|---|---|
| `src/ui/oe_timeline_layout.[ch]` (extended) | `OeSnapContext` (enabled, threshold_px, px_per_us, playhead_us, frame_interval_us, edges_us/n_edges) + `oe_timeline_snap_time`: nearest-wins with earlier-time tie-break, inclusive px-scaled band, overflow-safe distance math; `OE_TIMELINE_SNAP_THRESHOLD_PX` (8) mirrors the 6 px edge band. `clamp_move_position` no longer fuses neighbour-edge snapping. |
| `src/ui/oe_timeline.c` (changed) | `build_snap_context` collects same-track edges (skipping the dragged clip), the playhead when visible, and the rational frame interval; `update_drag` snaps MOVE/TRIM_IN/TRIM_OUT candidates before clamping; the preview stays the only thing snap touches — `commit_drag` is untouched. The snapping flag + `_set/get_snapping` seam default on. |
| `src/app/oe_undo_stack.[ch]` (changed) | `OE_UNDO_OP_RIPPLE_DELETE` records carry `OeRippleShift` entries (pre/post indices and positions per suffix clip) alongside the owned primary `OeClip`; `oe_edit_ripple_remove_clip` records-then-shifts; `apply_ripple_undo` / `apply_ripple_redo` replay through `oe_project_move_clip` / `_insert_clip` with pinned orderings. |
| `src/app/oe_command.c` (changed) | `edit.snap-toggle` registered before `OE_CMD_COUNT`, accel `s`, wired to the window's toggle handler. |
| `src/ui/oe_main_window.c` (changed) | The toggle handler flips the widget flag and reports "Snapping on/off"; the Edit menu renders a stateful check `GSimpleAction` synchronized from the widget so the checkbox always reflects the widget's truth; delete-selection routes through the ripple recorder and clears selection after the action and its undo/redo. |
| `tests/test_snap_ripple.c` | Eighteen GTK-free cases linking the layout core, model, stack, persistence, and session; virtual clock for auto-pause; `SDL_AUDIODRIVER=dummy` via the meson env. |
| `meson.build` | The 15th suite (`snap-ripple`) wired like `undo-stack`: 300 s timeout, dummy-driver env. |
| `docs/*` | Architecture "Snapping and ripple (Phase 7)" section, code-map rows, glossary "Editing terms (Phase 7)", this file. `project-format.md` unchanged — no schema bump. |

## 3. Block-by-block build walkthrough

**Snapping is a pure decision, so it is testable without a display.**
`OeSnapContext` carries everything the decision needs: whether
snapping is on, the threshold in pixels, the zoom (`px_per_us`) to
convert that threshold into microseconds, optional playhead and
frame-grid targets, and the neighbour edges (same-track only; the
dragged clip's own edges are excluded by the collector, never by the
decider). `oe_timeline_snap_time` scans the target set, keeps the
nearest within the band, and returns either the snapped time or the
candidate untouched. Because the decider never touches GTK or the
model, the suite exercises it directly — the widget feeds the same
struct the tests do.

**The threshold lives in screen space on purpose.** A time-space
threshold (say 50 ms) feels jittery at high zoom and dead at low
zoom. Pixels keep the magnetic feel constant: the band widens in
microseconds as you zoom out. The suite pins the arithmetic both
ways — the same 200 ms miss is raw zoomed in (80 ms band) and
snapped zoomed out (800 ms band).

**The clamp stays authoritative.** Snap runs first, then the
legality clamp: a candidate magnetized onto a neighbour edge lands
flush (legal), and a candidate the clamp rejects recovers exactly as
it did in Phase 4. The suite proves both directions, including a
trim where the snap proposes 2 s but the clip only owns 1.5 s of
source — the clamp wins. Rejected proposals vanish with no state
change; the model never sees an infeasible position.

**The ripple record is one command for a multi-clip edit.** The
recorder captures the primary clip first, removes it, then shifts
each surviving downstream clip left by the primary's duration,
moving clips in ascending index order (a left-moving clip can never
overlap its not-yet-moved right neighbour, so every intermediate
state is valid). Each shift is captured as pre/post positions AND
indices because removal renumbers: the clip at pre-index k+1 lands
at post-index k. Undo shifts the suffix back right in DESCENDING
order (restoring the right neighbour first leaves room), then
re-inserts the primary into its freed slot; redo removes and shifts
again ascending. One action = one record = one depth unit, so the
Phase 6 stack machinery (strict LIFO, depth 100, redo-branch
clearing) applies unmodified.

**The toggle is stateful through the widget, not the menu.** The
Edit-menu check row is a `GSimpleAction` whose state mirrors the
widget's session flag — the widget stays the single source of truth,
the menu never owns state, and the status bar reports "Snapping
on/off" through the same seam every other command uses.

## 4. C concepts in play

- **Struct-by-value contexts.** `OeSnapContext` is a plain struct
  passed by pointer, built fresh per drag frame — cheap, no
  ownership, and the `edges_us` array is borrowed, not owned.
- **Screen-space vs time-space units.** One zoom factor
  (`px_per_us`) converts the pixel threshold into a time band;
  integer microseconds keep the math exact (no floats in the
  decision).
- **Tie-breaking as a spec, not an accident.** Strict `<=`/`<`
  comparisons in the scan decide which target wins an exact tie;
  the tests pin the choice (earlier time) so it can never drift.
- **Composite commands.** The ripple record bundles several model
  mutations under one undo step — the command-object pattern paying
  off: undo/redo don't know or care that the action was compound.
- **Dual-generation indices.** Capturing pre AND post indices per
  suffix clip makes the record robust against the renumbering the
  primary removal performs; replay targets recorded indices instead
  of recomputing them.
- **Ordering as a correctness argument.** Ascending shifts for
  forward motion, descending for reverse — each invariant is one
  sentence long and enforced by construction, not by luck.

## 5. Ownership table

| Artifact | Owner | Lifetime |
|---|---|---|
| Snapping flag | `OeTimeline` (widget session state) | widget |
| `OeSnapContext` / edges array | built per drag frame by the widget | drag frame |
| Snap decision result | caller (the preview field) | drag frame |
| Ripple record (`OeUndoRecord`) | `OeUndoStack` (pushed by the recorder) | history |
| `OeRippleShift` array | owned by the record, freed with it | history |
| Model mutations | `OeProject` mutators only | immediate |

## 6. Call flow

Drag path (move, trim-in, trim-out):

```
gesture motion
  → oe_timeline_us_for_x (pointer time)
  → build_snap_context (same-track edges, playhead, frame interval)
  → oe_timeline_snap_time (context, candidate)   [pure]
  → oe_timeline_clamp_move_position / trim bounds  [pure, authoritative]
  → preview field + queue_draw                   [model untouched]

gesture release
  → commit_drag → oe_edit_move_clip / oe_edit_trim_clip (unchanged)
```

Ripple delete path:

```
delete-selection (window) or Edit > Delete
  → oe_edit_ripple_remove_clip (project, stack, track, index)
      → oe_project_get_clip (capture primary)
      → oe_project_remove_clip (primary; renumbers suffix)
      → for k = index .. count-1 (ascending)
          → oe_project_move_clip (k, position - duration)
          → record shift {pre_index = k+1, post_index = k, pre, post}
      → push ONE OE_UNDO_OP_RIPPLE_DELETE record

undo → apply_ripple_undo (descending shifts, then insert primary)
redo → apply_ripple_redo (remove primary, ascending shifts)
```

Toggle path:

```
accel 's' → edit.snap-toggle → window handler
  → oe_timeline_set_snapping (widget flag)
  → status reporter "Snapping on/off"
  → GSimpleAction state sync (Edit-menu checkbox)
```

## 7. Alternatives considered

**Keep fusing snapping into the clamp.** The Phase 4 clamp had
neighbour-edge magnetism built in. Keeping it would have saved a
seam but made the decision untestable headlessly, unconfigurable
(threshold, targets, toggle), and wrong for trims (which need
different candidate classes). Splitting legality (clamp) from
magnetism (snap) let each rule stay one pure function.

**Time-space threshold.** Simpler arithmetic, wrong feel: the
magnetic band should be a constant finger-width on screen, not a
constant duration. Pixels + `px_per_us` is the same arithmetic the
edge-band hit-test already uses.

**Ripple as N separate records.** Recording the remove and each
shift as independent MOVE records would have reused Phase 6
machinery verbatim but broken one-action/one-undo: a delete would
undo in k+1 steps, and eviction could strand the group mid-way. One
composite record keeps the user's mental model (one action, one
undo) and the stack's depth accounting aligned.

**Full-model snapshot for ripple.** The Phase 6 escape hatch exists
for hard-to-invert operations, but the ripple replay is expressible
through existing typed mutators with provable orderings — records
stay small (k+1 entries), and every intermediate state reuses the
model's own validation.

**Cross-track ripple.** Deleting a clip shifting all tracks is a
different (and rarer) semantic; Phase 7 pins same-track ripple as
the default NLE behavior and leaves cross-track for a later decision.

## 8. Bug log

**The suffix loop started at index 0.** The first recorder draft
iterated every post-removal clip from 0, not from the primary's old
slot. First-deletes passed by coincidence — after deleting index 0,
"every remaining clip" IS the suffix. Middle and last deletes
shifted clips to the primary's LEFT (their new positions went
negative and the mutators rejected them, logged as a CRITICAL and a
partial record). The middle-delete test caught it on first run; the
fix starts the loop at `clip_index` (where the suffix was
renumbered to) and derives recorded indices from that origin. The
bug log lesson: a case set with first/middle/last deletes is what
made the coincidence visible.

**Zoom scale confusion in tests.** An early draft mixed px/µs
scales (0.1 vs 0.0001), so some candidates sat outside the band
they claimed to test. Rewriting every case against one zoom (100
px/s → 80 ms band) made the band arithmetic legible again and
surfaced a tie-break case worth having: a frame boundary exactly
80 ms from an edge.

**A trim test asserting the wrong contract.** The first trim
snap-then-clamp case expected snap and clamp to agree at 2 s — but
clip B owns only 1.5 s of source, so the clamp correctly refuses.
The fixed test asserts the real contract (snap proposes 2 s, clamp
returns `source_out - 1`), which is a stronger statement than the
agreement case ever was.

## 9. What is next

Export remains the last headline item. Candidate refinements for a
future phase: trim-to-trim snap candidates (A's in-edge against B's
out-edge), ripple on insert (make room), cross-track ripple as a
modifier, and snap indicators (drawing which target captured the
drag). None of them change the record format — `.oe` stays v1.

## 10. Five review questions (with answers)

**Q1: Why does `oe_timeline_snap_time` take the whole context
instead of separate arguments?**
So the decision has one shape for every caller and every drag kind.
Adding a target class later (markers, tempo) means adding a context
field, not a new signature — and the tests build contexts exactly
like the widget does, so the tested decision is the shipped one.

**Q2: Why is the threshold inclusive at exactly the band edge?**
The band edge is the last distance a user would still call "close".
An exclusive boundary makes the 8 px feel like 7 px at the one
distance the math can express exactly; the suite pins 80000 µs
snaps, 80001 doesn't.

**Q3: How can undo replay move clips without ever failing typed
validation mid-way?**
Ordering. Undo applies descending (right neighbour first), redo
ascending (left first). A left-moving clip cannot overlap a
not-yet-moved right neighbour, and a right-moving clip cannot
overlap an already-restored right neighbour — every intermediate
state is one the model would have accepted live.

**Q4: Why record pre AND post indices for every suffix clip when
they differ by exactly one?**
Because "differs by exactly one" is a property of THIS record shape
(remove-one). Capturing both generations makes the record
self-describing and the replay indexing-independent — a future
record kind that renumbers differently still round-trips.

**Q5: Does the snapping toggle survive undo/redo? Does the ripple
record survive save/load?**
No and no — both by design. The snapping flag is widget session
state (like zoom and selection); it is never serialized. The ripple
record is history; history is never serialized either. A saved
project contains only format-v1 model state, which is why
`project-format.md` is untouched.
