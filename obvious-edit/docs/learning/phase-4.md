# Phase 4 — the timeline widget: seeing and touching the model

A guided walkthrough of Phase 4: what was built, why it is shaped this
way, and what to look at when you read the code.

## 1. Phase purpose

Phase 3 gave Obvious Edit a document — a project model that is fully
validated and fully tested but invisible. Phase 4 puts it on screen and
makes it touchable: a real timeline where clips appear, move, trim, and
refuse to do anything illegal, with the model still the single
authority.

Concretely, four things appear:

- `oe_project_trim_clip` — the first core mutator added by a UI phase:
  change a clip's source range in place, with typed validation against
  probed source durations and the same observer-once contract as every
  other mutator.
- `src/ui/oe_timeline_layout.[ch]` — the GTK-free math half of the
  timeline: zoom state, microsecond↔pixel round-trips, lane mapping,
  edge-band hit-testing, and the pure move/trim clamp functions. It
  links GLib only, so every line of it is unit-tested headlessly.
- `src/ui/oe_timeline.[ch]` — the widget half: a `GtkDrawingArea` that
  paints the model with Cairo, observes the project, and drives one
  shared drag state machine whose gestures end in model mutators.
- App wiring — Insert from Bin (Ctrl+E), Delete (selection.delete),
  zoom in/out commands plus Ctrl+wheel, and ruler-click playhead, all
  fulfilling command-registry promises Phase 1 made and left parked.

What Phase 4 deliberately does **not** add: no playback clock (the
playhead is session state; Phase 5 owns time), no undo/redo, no
snapping, and no razor/select tools — those keep their owning-phase
comments in the command table.

## 2. Per-file explanations

| File | Role |
|---|---|
| `src/core/oe_project.[ch]` (extended) | `oe_project_trim_clip(project, track, clip, in, out, err)` — validates in<out, AV bounds against the session's probed source-duration annotations, still-extension rule, overlap backstop; position unchanged; observer fires exactly once on success. |
| `src/ui/oe_timeline_layout.[ch]` | Zoom as `px_per_us`, x/us conversions with saturation at the timeline end, ruler/lane mapping, 6-px edge-grab hit-test, `clamp_move_position` (adjacency), `trim_bounds` (source limits). GTK-free, allocation-free. |
| `src/ui/oe_timeline.[ch]` | `OeTimeline` final type over `GtkDrawingArea`: observer snapshots from `oe_project_get_sequence`, Cairo painting (ruler, lanes, clips, audio strips, stills, hatched missing media, selection, playhead), one drag state machine, session zoom/playhead/selection, resolve + report seams. |
| `src/app/oe_command.[ch]` (extended) | `media.insert-from-bin` (Ctrl+E), `view.zoom-in` (Ctrl+=), `view.zoom-out` (Ctrl+-) registered with accelerators; enum order preserved — new IDs append before `SHOW_ABOUT`. |
| `src/ui/obvious-edit.css` (extended) | `.timeline` class: opaque base color and focus outline only — the widget paints its own palette with Cairo. |
| `src/ui/oe_main_window.c` (changed) | The timeline placeholder string is gone; the panel hosts the real widget. The window maps project media refs → session assets, annotates durations on import, wires resolve/report callbacks, and owns the four new command handlers. |
| `tests/test_timeline_layout.c` | Five cases over the pure math: zoom round-trips, lane mapping, hit-test bands, move clamps with adjacency, trim bounds. |
| `tests/test_project.c` (extended) | Four trim cases: valid edit, typed rejections with silent observers, still extension + overlap protection, observer-once counting. |
| `tests/test_shell_layout.c` (comment fix) | The header comment now says eight cases and lists `default-path` — the file was right, the comment was stale. |
| `docs/architecture.md` | New "The timeline widget (Phase 4)" layer rules; the fragile `oe_time.h` line-number citation now names the section. |

## 3. Block-by-block build walkthrough

**Trim first, because the widget commits through it.** A trim changes
`source_in_us` / `source_out_us` and keeps the position. The validator
order: track and clip must exist; `in < out`; for AV media the new
range must fit inside the annotated probed duration; stills may extend
freely (the uniform-duration rule — a still's source range *is* its
screen time); and the resulting timeline extent may not overlap a
neighbor. Every rejection is a typed `OE_PROJECT_ERROR` code and a
silent observer — failed mutations never notify, the contract from
Phase 3 held unchanged.

**Layout as a pure module.** Everything geometric lives in
`oe_timeline_layout`: `px_per_us` is the whole zoom model; `x_for_us`
saturates rather than overflows at the timeline end; lanes answer "which
track row is y in"; `hit_test` classifies a click as clip-body,
trim-in edge, or trim-out edge inside a 6-pixel grab band (bigger than
the visual line because a 1-px drag target is unusable); the clamp
functions take a candidate and return the nearest legal one. No
function here allocates, includes GTK, or knows the widget exists —
which is exactly why the test suite needs no display.

**The widget paints a copy, not the model.** `oe_timeline` registers
as the project's observer — the first production consumer of the
Phase 3 seam. Each notification (and each initial bind) pulls a fresh
deep-copy sequence and schedules a redraw; drawing reads only the
snapshot. Missing media renders as a hatched rect via a resolver
callback the window provides (backed by the session library), so a
draw never probes a file. Empty projects get a truthful empty state —
the placeholder string that Phase 1 drew is gone.

**One state machine behind three gestures.** Press classifies through
`hit_test`: a trim edge arms trim-in/trim-out, a clip body arms move,
the ruler arms the playhead, anything else clears the selection. Drag
previews are clamped through the layout functions and painted — the
model is untouched while dragging. Release commits once through
`oe_project_move_clip` / `oe_project_trim_clip`; a typed rejection is
reported through the window's status seam and the preview snaps back.
No code path writes the model directly from a gesture.

**The app layer closes the loop.** Insert from Bin takes the bin
selection, adds the media ref to the project if new, inserts at the
playhead on the first kind-matching track, and advances the session
playhead. Delete removes the selected clip and clears the selection.
Zoom doubles or halves `px_per_us` around an anchor (pointer for
Ctrl+wheel, widget center for the commands) — session state, never
serialized. The window also does the bookkeeping the model must not:
mapping refs to assets and annotating probed durations on import.

## 4. C concepts in play

- **Callback seams over inheritance.** The widget takes two plain
  function pointers — resolve (ref → media facts) and report (status
  line) — instead of signaling into concrete types. The window owns
  the closures; the widget stays testable and decoupled.
- **Observer + deep copy.** The snapshot pattern from Phase 3 pays off:
  the draw function can never race a mutation because it reads a copy
  taken under the model's lock-free main-thread contract.
- **Single-writer state machines.** One enum-typed drag state, one
  press path that arms it, one release path that disarms it. The bugs
  this prevents (double commits, orphaned drags) are the classic cost
  of scattering booleans across gesture handlers.
- **Saturation arithmetic.** Time→pixel conversion clamps at the
  timeline end instead of wrapping, so a huge `source_out` can never
  scroll the ruler into negative-x nonsense.
- **Session state vs model state.** Playhead, zoom, and selection are
  C structs in the widget, deliberately absent from `OeSequence` —
  the type system documents what serializes and what does not.

## 5. Ownership table

| Object | Owner | Lifetime |
|---|---|---|
| `OeTimeline` widget | the window's timeline panel (GTK owns the tree) | window |
| Sequence snapshots | the widget; replaced on every observer fire | between redraws |
| `px_per_us`, playhead, selection | the widget (session state) | session |
| Media-ref → asset maps | the window (GHashTable, cleared in dispose) | session |
| Source-duration annotations | the project records, session-only, never serialized | session |
| Drag state | the widget's single state machine | one gesture |
| Trim validation rules | the core model (`oe_project_trim_clip`) | permanent |

Destruction order in the window: detach the timeline observer before
the project dies, free the ref maps in dispose, let GTK take the
widget tree.

## 6. Call flow

**Move.** press on clip body → state machine arms move → drag computes
candidate through `clamp_move_position` (nearest legal slot, adjacency
aware) → preview painted from the candidate → release commits
`oe_project_move_clip` → observer fires once → snapshot replaced →
redraw. Rejection: typed error → reporter seam → status line →
snap-back preview discarded.

**Trim.** press inside a 6-px edge band → trim-in or trim-out armed →
drag computes `trim_bounds` (source limits; stills unbounded ahead) →
preview shows the clamped extent → release commits
`oe_project_trim_clip`. Missing media refuses to arm at all — hatched
clips are read-only by policy.

**Insert from Bin.** Ctrl+E or Edit menu → handler reads the bin
selection → `oe_project_add_media` if the ref is new → the first track
whose kind matches the asset gets `oe_project_insert_clip` at the
playhead → playhead advances past the new clip → observer redraw. An
empty selection reports instead of guessing.

**Delete.** Delete key → `selection.delete` action → handler asks the
widget for the selection → `oe_project_remove_clip` → selection
cleared → observer redraw. No selection: reporter says so, nothing
changes.

**Zoom.** Ctrl+= / Ctrl+- / Ctrl+wheel → double or halve `px_per_us`
clamped to sane limits → anchor time held fixed on screen → redraw.
No model involvement anywhere in the path.

## 7. Alternatives considered

- **Geometry inside the widget.** Keeping the math in `oe_timeline.c`
  would have saved a file but made every rule untestable without a
  display. The GTK-free layout module is the same move as Phase 1's
  GTK-free command registry and shell layout: if it can be tested
  headlessly, it must.
- **Playhead in the model.** Serializing the playhead would put view
  state into the document and hand Phase 5 a migration problem. It is
  session state; the clock will own it next phase.
- **Durations copied into clip records.** Instead, probed durations are
  session-only annotations on media records: the project file keeps the
  Phase 3 schema untouched, and a stale annotation can never disagree
  with a serialized clip.
- **Snapping during drags.** Deferred with the owning-phase comment —
  adding snap logic inside the clamp functions now would couple two
  features that Phase 6 wants to evolve together.
- **Overlap leniency on trim.** The spec could be read to allow a trim
  that overlaps a neighbor; the overlap backstop stays because a legal
  model after every mutation is worth more than drag convenience.

## 8. Bug log

- **Zoom arithmetic off by 100×.** The first layout fixture used
  `0.01` px/µs where the intended 100 px/s is `0.0001`. The test
  failed with numbers that looked plausible; recomputing from units
  caught it. Lesson: write the units in the fixture names.
- **An over-clever wobble assertion.** A case asserted on a rounding
  "wobble" that depends on the compiler's fp evaluation; replaced with
  a stable rounding test. Tests assert contracts, not accidents.
- **Stale header, wrong count.** `test_shell_layout.c` grew an eighth
  case (`default-path`) and the header still said seven — a doc bug
  that survives any test run. Count assertions in comments age; this
  one is fixed.
- **Malformed scripted edits.** Several edit-tool insertions landed at
  wrong anchors early in the phase (split declarations, a placeholder
  in a header). All were repaired with exact-anchor edits; the build
  and the format gate both pass on the final tree.

## 9. What is next

Phase 5 adds the playback clock and audio output wiring: the playhead
stops being a view artifact and becomes time. Undo/redo follows around
the command seams, then snapping and ripple edits on top of the same
clamp functions. `src/core/` remains the foundation; the widget gained
no privileges this phase — it commits through the same mutators the
tests exercise.

## 10. Five review questions (with answers)

1. **Why does the widget hold a deep copy instead of a model pointer?**
   The draw path must be immune to mid-frame mutation. Deep copies turn
   that from a discipline into a fact: between observer fires the
   widget physically cannot see a half-applied change.
2. **Why can a still be trimmed past its probed duration — but an AV
   clip cannot?** A still has no source clock; its source range *is*
   its screen duration (the uniform-duration rule). AV media has real
   frames; a range past the probe would promise pixels that do not
   exist.
3. **How does a rejected drag avoid corrupting anything?** Drags only
   paint clamped candidates; the model is written once, at release,
   through a validating mutator. A rejection means the mutator said no
   — the preview is discarded and the next redraw paints the model.
4. **Why are the new commands appended before `OE_CMD_SHOW_ABOUT`
   instead of after it?** The table is indexed by ID and the enum order
   is permanent API: appending before the sentinel keeps existing IDs
   stable while `OE_CMD_COUNT` still counts every entry.
5. **What keeps the window from re-probing media on every draw?**
   Nothing probes during draws at all: the window resolves refs to
   session assets up front, the widget asks the resolver, and the
   library's statuses (OK / MISSING / UNSUPPORTED) are the only source
   of truth for rendering and trim refusal.
